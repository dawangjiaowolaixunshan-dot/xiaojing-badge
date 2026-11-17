#include "tts.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "mic.h"
#include "settings.h"

static const char *TAG = "tts";
#define URI "wss://dashscope.aliyuncs.com/api-ws/v1/inference"
#define MODEL "cosyvoice-v2"
#define VOICE "longxiaochun_v2"
#define AUDIO_BUF (MIC_RATE * 2 * 20)   /* 20 s of PCM ahead of the speaker */
#define TEXT_CAP 8192
#define BLOCK 320

#define EV_STARTED BIT0
#define EV_FINISHED BIT1
#define EV_FAILED BIT2
#define EV_NO_MORE BIT3
#define EV_STOP BIT4
#define EV_IDLE BIT5

static StreamBufferHandle_t s_audio;
static EventGroupHandle_t s_ev;
static SemaphoreHandle_t s_mu;
static char *s_text, *s_msg;   /* text waiting to be sent; incoming message */
static size_t s_text_len, s_msg_len;
static char s_task_id[33];
static bool s_enabled = true;
static volatile bool s_flush;
static volatile int s_level;   /* loudness of what's playing, 0..100 */

/* ---- the speaker ---- */

static void player_task(void *arg)
{
    int16_t *mono = heap_caps_malloc(BLOCK * 2, MALLOC_CAP_INTERNAL);
    int16_t *stereo = heap_caps_malloc(BLOCK * 4, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    esp_codec_dev_handle_t spk = mic_speaker();
    for (;;) {
        size_t n = xStreamBufferReceive(s_audio, mono, BLOCK * 2, portMAX_DELAY);
        if (s_flush) continue;
        n /= 2;
        int64_t sq = 0;
        for (size_t i = 0; i < n; i++) {
            stereo[2 * i] = stereo[2 * i + 1] = mono[i];
            sq += (int32_t)mono[i] * mono[i];
        }
        int rms = n ? (int)sqrtf((float)sq / n) : 0;
        int lvl = rms < 200 ? 0 : (int)(28.0f * log10f(rms / 200.0f));
        s_level = lvl > 100 ? 100 : lvl;
        if (spk && n) esp_codec_dev_write(spk, stereo, n * 4);
    }
}

/* ---- the socket ---- */

static void ws_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    esp_websocket_event_data_t *d = data;
    if (id == WEBSOCKET_EVENT_DATA && d->op_code == 0x2) {   /* audio */
        if (!(xEventGroupGetBits(s_ev) & EV_STOP) && d->data_len > 0) {
            xStreamBufferSend(s_audio, d->data_ptr, d->data_len, pdMS_TO_TICKS(2000));
        }
    } else if (id == WEBSOCKET_EVENT_DATA && d->op_code == 0x1) {
        if (d->payload_offset == 0) s_msg_len = 0;
        if (s_msg_len + d->data_len < 4096) {
            memcpy(s_msg + s_msg_len, d->data_ptr, d->data_len);
            s_msg_len += d->data_len;
        }
        if (d->payload_offset + d->data_len < d->payload_len) return;
        s_msg[s_msg_len] = '\0';
        cJSON *root = cJSON_Parse(s_msg);
        cJSON *header = cJSON_GetObjectItem(root, "header");
        const char *event = cJSON_GetStringValue(cJSON_GetObjectItem(header, "event"));
        if (event && !strcmp(event, "task-started")) xEventGroupSetBits(s_ev, EV_STARTED);
        else if (event && !strcmp(event, "task-finished")) xEventGroupSetBits(s_ev, EV_FINISHED);
        else if (event && !strcmp(event, "task-failed")) {
            ESP_LOGE(TAG, "task failed: %s %s",
                     cJSON_GetStringValue(cJSON_GetObjectItem(header, "error_code")) ?: "",
                     cJSON_GetStringValue(cJSON_GetObjectItem(header, "error_message")) ?: "");
            xEventGroupSetBits(s_ev, EV_FAILED);
        }
        cJSON_Delete(root);
    } else if (id == WEBSOCKET_EVENT_DISCONNECTED || id == WEBSOCKET_EVENT_ERROR) {
        if (!(xEventGroupGetBits(s_ev) & EV_FINISHED)) xEventGroupSetBits(s_ev, EV_FAILED);
    }
}

static void send_cmd(esp_websocket_client_handle_t ws, const char *action, const char *text)
{
    cJSON *root = cJSON_CreateObject(), *header = cJSON_AddObjectToObject(root, "header");
    cJSON_AddStringToObject(header, "action", action);
    cJSON_AddStringToObject(header, "task_id", s_task_id);
    cJSON_AddStringToObject(header, "streaming", "duplex");
    cJSON *payload = cJSON_AddObjectToObject(root, "payload");
    if (!strcmp(action, "run-task")) {
        cJSON_AddStringToObject(payload, "task_group", "audio");
        cJSON_AddStringToObject(payload, "task", "tts");
        cJSON_AddStringToObject(payload, "function", "SpeechSynthesizer");
        cJSON_AddStringToObject(payload, "model", MODEL);
        cJSON *p = cJSON_AddObjectToObject(payload, "parameters");
        cJSON_AddStringToObject(p, "text_type", "PlainText");
        cJSON_AddStringToObject(p, "voice", VOICE);
        cJSON_AddStringToObject(p, "format", "pcm");
        cJSON_AddNumberToObject(p, "sample_rate", MIC_RATE);
        cJSON_AddNumberToObject(p, "volume", 60);
    }
    cJSON *input = cJSON_AddObjectToObject(payload, "input");
    if (text) cJSON_AddStringToObject(input, "text", text);
    char *msg = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (msg) {
        esp_websocket_client_send_text(ws, msg, strlen(msg), pdMS_TO_TICKS(3000));
        cJSON_free(msg);
    }
}

static void tts_task(void *arg)
{
    char *key = heap_caps_malloc(256, MALLOC_CAP_SPIRAM);
    char *headers = heap_caps_malloc(320, MALLOC_CAP_SPIRAM);
    char *piece = heap_caps_malloc(TEXT_CAP, MALLOC_CAP_SPIRAM);
    settings_get_key(KEY_BAILIAN, key, 256);
    snprintf(headers, 320, "Authorization: bearer %s\r\nX-DashScope-DataInspection: enable\r\n", key);
    memset(key, 0, 256);
    esp_websocket_client_config_t cfg = {
        .uri = URI,
        .headers = headers,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 4096,
        .task_stack = 6144,
        .network_timeout_ms = 10000,
        .disable_auto_reconnect = true,
    };
    esp_websocket_client_handle_t ws = esp_websocket_client_init(&cfg);
    esp_websocket_register_events(ws, WEBSOCKET_EVENT_ANY, ws_event, NULL);
    int64_t t0 = esp_log_timestamp();
    bool sent_any = false;
    const char *why = "connect";
    if (esp_websocket_client_start(ws) != ESP_OK) goto fail;
    for (int i = 0; i < 100 && !esp_websocket_client_is_connected(ws); i++) {
        if (xEventGroupGetBits(s_ev) & (EV_STOP | EV_FAILED)) goto fail;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (!esp_websocket_client_is_connected(ws)) goto fail;
    why = "start";
    send_cmd(ws, "run-task", NULL);
    if (!(xEventGroupWaitBits(s_ev, EV_STARTED | EV_FAILED | EV_STOP, pdFALSE, pdFALSE, pdMS_TO_TICKS(5000)) & EV_STARTED)) goto fail;
    ESP_LOGI(TAG, "started in %d ms", (int)(esp_log_timestamp() - t0));

    for (;;) {   /* send the text as it comes, a few times a second */
        EventBits_t b = xEventGroupGetBits(s_ev);
        if (b & (EV_STOP | EV_FAILED)) goto done;
        xSemaphoreTake(s_mu, portMAX_DELAY);
        size_t n = s_text_len;
        memcpy(piece, s_text, n);
        piece[n] = '\0';
        s_text_len = 0;
        xSemaphoreGive(s_mu);
        if (n) {
            send_cmd(ws, "continue-task", piece);
            sent_any = true;
        } else if (b & EV_NO_MORE) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(150));
    }
    if (!sent_any) goto done;
    send_cmd(ws, "finish-task", NULL);
    xEventGroupWaitBits(s_ev, EV_FINISHED | EV_FAILED | EV_STOP, pdFALSE, pdFALSE, pdMS_TO_TICKS(60000));
    ESP_LOGI(TAG, "done %d ms after start", (int)(esp_log_timestamp() - t0));
    goto done;
fail:
    ESP_LOGW(TAG, "%s failed%s", why, xEventGroupGetBits(s_ev) & EV_STOP ? " (stopped)" : "");
done:
    esp_websocket_client_destroy(ws);
    memset(headers, 0, 320);
    free(headers);
    free(key);
    free(piece);
    xEventGroupSetBits(s_ev, EV_IDLE);
    vTaskDelete(NULL);
}

void tts_init(void)
{
    s_ev = xEventGroupCreate();
    xEventGroupSetBits(s_ev, EV_IDLE);
    s_mu = xSemaphoreCreateMutex();
    static StaticStreamBuffer_t sb;
    uint8_t *storage = heap_caps_malloc(AUDIO_BUF + 1, MALLOC_CAP_SPIRAM);
    s_audio = xStreamBufferCreateStatic(AUDIO_BUF, BLOCK * 2, storage, &sb);
    s_text = heap_caps_malloc(TEXT_CAP, MALLOC_CAP_SPIRAM);
    s_msg = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
    uint8_t on = 1;
    if (settings_get_blob("voice", &on, 1)) s_enabled = on;
    xTaskCreatePinnedToCore(player_task, "player", 3072, NULL, 7, NULL, 1);
}

bool tts_begin(void)
{
    if (!s_enabled || !(xEventGroupGetBits(s_ev) & EV_IDLE)) {
        ESP_LOGW(TAG, "not speaking: %s", s_enabled ? "busy" : "voice off");
        return false;
    }
    ESP_LOGI(TAG, "begin");
    xEventGroupClearBits(s_ev, 0xff);
    s_text_len = 0;
    s_flush = false;
    for (int i = 0; i < 16; i++) sprintf(&s_task_id[2 * i], "%02x", (unsigned)(esp_random() & 0xff));
    if (xTaskCreatePinnedToCore(tts_task, "tts", 6144, NULL, 5, NULL, 0) != pdPASS) {
        xEventGroupSetBits(s_ev, EV_IDLE);
        return false;
    }
    return true;
}

void tts_feed(const char *text)
{
    size_t n = strlen(text);
    xSemaphoreTake(s_mu, portMAX_DELAY);
    if (s_text_len + n < TEXT_CAP) {
        memcpy(s_text + s_text_len, text, n);
        s_text_len += n;
    }
    xSemaphoreGive(s_mu);
}

void tts_finish(void) { xEventGroupSetBits(s_ev, EV_NO_MORE); }

void tts_stop(void)
{
    /* The player drains what's queued without playing it until the next
     * tts_begin(); a reset isn't allowed while it waits on the buffer. */
    xEventGroupSetBits(s_ev, EV_STOP);
    s_flush = true;
}

bool tts_enabled(void) { return s_enabled; }

int tts_level(void) { return tts_active() ? s_level : 0; }

bool tts_active(void)
{
    return !s_flush && (!(xEventGroupGetBits(s_ev) & EV_IDLE) || xStreamBufferBytesAvailable(s_audio) > 0);
}

void tts_set_enabled(bool on)
{
    s_enabled = on;
    uint8_t v = on;
    settings_set_blob("voice", &v, 1);
    if (!on) tts_stop();
}
