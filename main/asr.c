#include "asr.h"

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
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "mic.h"
#include "net.h"
#include "settings.h"

static const char *TAG = "asr";
#define URI "wss://dashscope.aliyuncs.com/api-ws/v1/inference"
#define MODEL "paraformer-realtime-v2"
#define AUDIO_BUF (MIC_RATE * 2 * 30)   /* 30 s of PCM, in PSRAM */
#define CHUNK 3200                      /* 100 ms per frame, as DashScope suggests */
#define TEXT_CAP 2048
#define MSG_CAP 16384

#define EV_STARTED BIT0
#define EV_FINISHED BIT1
#define EV_FAILED BIT2
#define EV_STOP BIT3
#define EV_CANCEL BIT4
#define EV_IDLE BIT5

static StreamBufferHandle_t s_audio;
static EventGroupHandle_t s_ev;
static asr_text_cb_t s_cb;
static const char *s_langs = "\"zh\",\"en\"";   /* JSON array body for language_hints */
static char s_task_id[33];
static char *s_done, *s_text, *s_msg;   /* finished sentences, shown text, incoming message */
static size_t s_msg_len;

static void audio_sink(const int16_t *pcm, size_t samples)
{
    xStreamBufferSend(s_audio, pcm, samples * sizeof(int16_t), 0);   /* full: drop */
}

static void handle_message(const char *json)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) return;
    cJSON *header = cJSON_GetObjectItem(root, "header");
    const char *event = cJSON_GetStringValue(cJSON_GetObjectItem(header, "event"));
    if (!event) {
    } else if (!strcmp(event, "task-started")) {
        xEventGroupSetBits(s_ev, EV_STARTED);
    } else if (!strcmp(event, "result-generated")) {
        cJSON *sentence = cJSON_GetObjectItem(cJSON_GetObjectItem(cJSON_GetObjectItem(root, "payload"), "output"), "sentence");
        const char *text = cJSON_GetStringValue(cJSON_GetObjectItem(sentence, "text"));
        cJSON *end = cJSON_GetObjectItem(sentence, "sentence_end");
        bool ended = cJSON_IsTrue(end) || cJSON_IsNumber(cJSON_GetObjectItem(sentence, "end_time"));
        if (text && !cJSON_GetObjectItem(sentence, "heartbeat")) {
            snprintf(s_text, TEXT_CAP, "%s%s", s_done, text);
            if (ended) strlcpy(s_done, s_text, TEXT_CAP);
            if (s_cb) s_cb(s_text, false);
        }
    } else if (!strcmp(event, "task-finished")) {
        xEventGroupSetBits(s_ev, EV_FINISHED);
    } else if (!strcmp(event, "task-failed")) {
        ESP_LOGE(TAG, "task failed: %s %s",
                 cJSON_GetStringValue(cJSON_GetObjectItem(header, "error_code")) ?: "",
                 cJSON_GetStringValue(cJSON_GetObjectItem(header, "error_message")) ?: "");
        xEventGroupSetBits(s_ev, EV_FAILED);
    }
    cJSON_Delete(root);
}

static void ws_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    esp_websocket_event_data_t *d = data;
    if (id == WEBSOCKET_EVENT_DATA && d->op_code == 0x1) {
        if (d->payload_offset == 0) s_msg_len = 0;
        if (s_msg_len + d->data_len < MSG_CAP) {
            memcpy(s_msg + s_msg_len, d->data_ptr, d->data_len);
            s_msg_len += d->data_len;
        }
        if (d->payload_offset + d->data_len >= d->payload_len) {
            s_msg[s_msg_len] = '\0';
            handle_message(s_msg);
            s_msg_len = 0;
        }
    } else if (id == WEBSOCKET_EVENT_DISCONNECTED || id == WEBSOCKET_EVENT_ERROR) {
        if (!(xEventGroupGetBits(s_ev) & EV_FINISHED)) xEventGroupSetBits(s_ev, EV_FAILED);
    }
}

static void send_json(esp_websocket_client_handle_t ws, const char *action, bool run)
{
    char *msg = heap_caps_malloc(768, MALLOC_CAP_SPIRAM);
    if (run) {
        snprintf(msg, 768,
                 "{\"header\":{\"action\":\"run-task\",\"task_id\":\"%s\",\"streaming\":\"duplex\"},"
                 "\"payload\":{\"task_group\":\"audio\",\"task\":\"asr\",\"function\":\"recognition\","
                 "\"model\":\"" MODEL "\",\"parameters\":{\"format\":\"pcm\",\"sample_rate\":%d,"
                 "\"language_hints\":[%s]},\"input\":{}}}",
                 s_task_id, MIC_RATE, s_langs);
    } else {
        snprintf(msg, 768, "{\"header\":{\"action\":\"%s\",\"task_id\":\"%s\",\"streaming\":\"duplex\"},"
                 "\"payload\":{\"input\":{}}}", action, s_task_id);
    }
    esp_websocket_client_send_text(ws, msg, strlen(msg), pdMS_TO_TICKS(3000));
    free(msg);
}

static void asr_task(void *arg)
{
    char *key = heap_caps_malloc(256, MALLOC_CAP_SPIRAM);
    char *headers = heap_caps_malloc(320, MALLOC_CAP_SPIRAM);
    uint8_t *chunk = heap_caps_malloc(CHUNK, MALLOC_CAP_SPIRAM);
    bool ok = false;
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
        .reconnect_timeout_ms = 60000,
        .disable_auto_reconnect = true,
    };
    esp_websocket_client_handle_t ws = NULL;
    int64_t t0 = esp_log_timestamp();
    /* A weak signal drops connects now and then; the audio waits in the
     * buffer meanwhile, so one more try costs nothing that was said. */
    for (int attempt = 0; attempt < 2 && !(xEventGroupGetBits(s_ev) & EV_CANCEL); attempt++) {
        if (ws) {
            esp_websocket_client_destroy(ws);
            ESP_LOGW(TAG, "connect failed; trying again");
        }
        xEventGroupClearBits(s_ev, EV_FAILED);
        ws = esp_websocket_client_init(&cfg);
        esp_websocket_register_events(ws, WEBSOCKET_EVENT_ANY, ws_event, NULL);
        if (esp_websocket_client_start(ws) != ESP_OK) continue;
        for (int i = 0; i < 100 && !esp_websocket_client_is_connected(ws); i++) {
            if (xEventGroupGetBits(s_ev) & (EV_CANCEL | EV_FAILED)) break;
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        if (esp_websocket_client_is_connected(ws)) break;
    }
    if (!ws || !esp_websocket_client_is_connected(ws)) goto done;
    send_json(ws, NULL, true);
    EventBits_t b = xEventGroupWaitBits(s_ev, EV_STARTED | EV_FAILED | EV_CANCEL, pdFALSE, pdFALSE, pdMS_TO_TICKS(5000));
    if (!(b & EV_STARTED)) goto done;
    ESP_LOGI(TAG, "started in %d ms", (int)(esp_log_timestamp() - t0));

    for (;;) {   /* stream audio until the button is let go, then what's left */
        bool stopping = xEventGroupGetBits(s_ev) & (EV_STOP | EV_CANCEL);
        size_t n = xStreamBufferReceive(s_audio, chunk, CHUNK, pdMS_TO_TICKS(stopping ? 0 : 120));
        if (n) esp_websocket_client_send_bin(ws, (const char *)chunk, n, pdMS_TO_TICKS(3000));
        if (stopping && !n) break;
        if (xEventGroupGetBits(s_ev) & EV_FAILED) goto done;
    }
    if (xEventGroupGetBits(s_ev) & EV_CANCEL) goto done;
    send_json(ws, "finish-task", false);
    b = xEventGroupWaitBits(s_ev, EV_FINISHED | EV_FAILED | EV_CANCEL, pdFALSE, pdFALSE, pdMS_TO_TICKS(6000));
    ok = b & EV_FINISHED;
    ESP_LOGI(TAG, "finished %s, %d ms after start", ok ? "ok" : "badly", (int)(esp_log_timestamp() - t0));
done:
    if (!ok && !(xEventGroupGetBits(s_ev) & EV_CANCEL)) {
        ESP_LOGW(TAG, "recognition failed");
    }
    mic_stop();
    /* The text first: closing the socket takes about a second. */
    if (!(xEventGroupGetBits(s_ev) & EV_CANCEL) && s_cb) {
        s_cb(ok ? s_text : NULL, true);   /* NULL: failed */
    }
    if (ws) esp_websocket_client_destroy(ws);
    memset(headers, 0, 320);
    free(headers);
    free(key);
    free(chunk);
    net_awake(false);
    xEventGroupSetBits(s_ev, EV_IDLE);
    vTaskDelete(NULL);
}

bool asr_begin(asr_text_cb_t cb) { return asr_begin_lang(cb, "\"zh\",\"en\""); }

bool asr_begin_lang(asr_text_cb_t cb, const char *langs)
{
    s_langs = langs;
    if (!s_ev) {
        s_ev = xEventGroupCreate();
        xEventGroupSetBits(s_ev, EV_IDLE);
        static StaticStreamBuffer_t sb;
        uint8_t *storage = heap_caps_malloc(AUDIO_BUF + 1, MALLOC_CAP_SPIRAM);
        s_audio = xStreamBufferCreateStatic(AUDIO_BUF, 1, storage, &sb);
        s_done = heap_caps_calloc(1, TEXT_CAP, MALLOC_CAP_SPIRAM);
        s_text = heap_caps_calloc(1, TEXT_CAP, MALLOC_CAP_SPIRAM);
        s_msg = heap_caps_malloc(MSG_CAP, MALLOC_CAP_SPIRAM);
    }
    if (!(xEventGroupWaitBits(s_ev, EV_IDLE, pdFALSE, pdTRUE, 0) & EV_IDLE)) {
        return false;   /* the last one is still winding down */
    }
    xEventGroupClearBits(s_ev, 0xff);
    xStreamBufferReset(s_audio);
    s_done[0] = s_text[0] = '\0';
    s_cb = cb;
    for (int i = 0; i < 16; i++) sprintf(&s_task_id[2 * i], "%02x", (unsigned)(esp_random() & 0xff));
    mic_start(audio_sink);
    net_awake(true);
    if (xTaskCreatePinnedToCore(asr_task, "asr", 6144, NULL, 5, NULL, 0) != pdPASS) {
        mic_stop();
        net_awake(false);
        xEventGroupSetBits(s_ev, EV_IDLE);
        return false;
    }
    return true;
}

void asr_end(void)
{
    mic_stop();
    if (s_ev) xEventGroupSetBits(s_ev, EV_STOP);
}

void asr_cancel(void)
{
    mic_stop();
    if (s_ev) xEventGroupSetBits(s_ev, EV_CANCEL);
}
