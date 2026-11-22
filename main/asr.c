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
#define CHUNK 1600                      /* 50 ms per frame: friendlier to mobile hotspot MTU */
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
static volatile uint32_t s_gen;         /* ignores late events from async-destroying old sockets */
static char s_err[48];

static void set_err(const char *msg)
{
    if (!s_err[0] && msg && msg[0]) strlcpy(s_err, msg, sizeof(s_err));
}

static void ws_destroy_task(void *arg)
{
    esp_websocket_client_handle_t ws = (esp_websocket_client_handle_t)arg;
    if (ws) esp_websocket_client_destroy(ws);
    vTaskDelete(NULL);
}

static void destroy_ws_async(esp_websocket_client_handle_t ws)
{
    if (!ws) return;
    if (xTaskCreatePinnedToCore(ws_destroy_task, "asr_cls", 4096, ws, 2, NULL, 0) != pdPASS) {
        esp_websocket_client_destroy(ws);
    }
}

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
        set_err("语音服务拒绝，再试一次");
        xEventGroupSetBits(s_ev, EV_FAILED);
    }
    cJSON_Delete(root);
}

static void ws_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if ((uint32_t)(uintptr_t)arg != s_gen) return;
    esp_websocket_event_data_t *d = data;
    if (id == WEBSOCKET_EVENT_DATA && (d->op_code == 0x1 || d->op_code == 0x0)) {
        if (d->op_code == 0x1 && d->payload_offset == 0) s_msg_len = 0;
        if (s_msg_len + d->data_len < MSG_CAP) {
            memcpy(s_msg + s_msg_len, d->data_ptr, d->data_len);
            s_msg_len += d->data_len;
        }
        if (d->payload_offset + d->data_len >= d->payload_len) {
            s_msg[s_msg_len] = '\0';
            handle_message(s_msg);
            s_msg_len = 0;
        }
    } else if (id == WEBSOCKET_EVENT_DATA && d->op_code == 0x8) {
        int code = d->data_len >= 2 ? (((uint8_t)d->data_ptr[0] << 8) | (uint8_t)d->data_ptr[1]) : 0;
        ESP_LOGW(TAG, "ws close %d: %.*s", code, d->data_len > 2 ? d->data_len - 2 : 0,
                 d->data_len > 2 ? d->data_ptr + 2 : "");
        if (!(xEventGroupGetBits(s_ev) & EV_FINISHED)) {
            set_err("语音连接被关闭");
            xEventGroupSetBits(s_ev, EV_FAILED);
        }
    } else if (id == WEBSOCKET_EVENT_DISCONNECTED || id == WEBSOCKET_EVENT_ERROR) {
        ESP_LOGW(TAG, "ws ev=%d http=%d err=%d", (int)id,
                 d ? d->error_handle.esp_ws_handshake_status_code : -1,
                 d ? (int)d->error_handle.error_type : -1);
        if (!(xEventGroupGetBits(s_ev) & EV_FINISHED)) {
            set_err("语音连接中断");
            xEventGroupSetBits(s_ev, EV_FAILED);
        }
    }
}

static void send_json(esp_websocket_client_handle_t ws, const char *action, bool run)
{
    char *msg = heap_caps_malloc(768, MALLOC_CAP_SPIRAM);
    if (!msg) return;
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
    char *key = heap_caps_calloc(1, 256, MALLOC_CAP_SPIRAM);
    char *headers = heap_caps_calloc(1, 320, MALLOC_CAP_SPIRAM);
    uint8_t *chunk = heap_caps_malloc(CHUNK, MALLOC_CAP_SPIRAM);
    esp_websocket_client_handle_t ws = NULL;
    bool ok = false;
    if (!key || !headers || !chunk || !settings_get_key(KEY_BAILIAN, key, 256)) {
        ESP_LOGW(TAG, "missing key or memory");
        goto done;
    }
    snprintf(headers, 320, "Authorization: bearer %s\r\nX-DashScope-DataInspection: enable\r\n", key);
    memset(key, 0, 256);

    uint32_t gen = (uint32_t)(uintptr_t)arg;
    esp_websocket_client_config_t cfg = {
        .uri = URI,
        .headers = headers,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 4096,
        .task_stack = 6144,
        .network_timeout_ms = 12000,
        .reconnect_timeout_ms = 60000,
        .disable_auto_reconnect = true,
    };
    int64_t t0 = esp_log_timestamp();
    /* Mobile hotspots can take 4-7 s for cold DNS+TLS; allow 9 s per try and
     * hand failed sockets to async cleanup so retries & next presses stay snappy. */
    for (int attempt = 0; attempt < 2 && !(xEventGroupGetBits(s_ev) & EV_CANCEL); attempt++) {
        if (ws) {
            destroy_ws_async(ws);
            ws = NULL;
            s_err[0] = '\0';
            ESP_LOGW(TAG, "connect failed; trying again");
        }
        xEventGroupClearBits(s_ev, EV_FAILED);
        ws = esp_websocket_client_init(&cfg);
        if (!ws) continue;
        esp_websocket_register_events(ws, WEBSOCKET_EVENT_ANY, ws_event, (void *)(uintptr_t)gen);
        if (esp_websocket_client_start(ws) != ESP_OK) continue;
        for (int i = 0; i < 180 && !esp_websocket_client_is_connected(ws); i++) {
            if (xEventGroupGetBits(s_ev) & (EV_CANCEL | EV_FAILED)) break;
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        if (esp_websocket_client_is_connected(ws)) break;
    }
    if (!ws || !esp_websocket_client_is_connected(ws)) {
        set_err("语音连接超时");
        goto done;
    }
    send_json(ws, NULL, true);
    EventBits_t b = xEventGroupWaitBits(s_ev, EV_STARTED | EV_FAILED | EV_CANCEL, pdFALSE, pdFALSE, pdMS_TO_TICKS(8000));
    if (!(b & EV_STARTED)) {
        set_err("语音握手超时");
        goto done;
    }
    ESP_LOGI(TAG, "started in %d ms", (int)(esp_log_timestamp() - t0));

    for (;;) {   /* stream audio until the button is let go, then what's left */
        bool stopping = xEventGroupGetBits(s_ev) & (EV_STOP | EV_CANCEL);
        size_t n = xStreamBufferReceive(s_audio, chunk, CHUNK, pdMS_TO_TICKS(stopping ? 0 : 100));
        if (n) {
            if (esp_websocket_client_send_bin(ws, (const char *)chunk, n, pdMS_TO_TICKS(5000)) < 0) {
                set_err("语音发送中断");
                goto done;
            }
        }
        if (stopping && !n) break;
        if (xEventGroupGetBits(s_ev) & EV_FAILED) goto done;
    }
    if (xEventGroupGetBits(s_ev) & EV_CANCEL) goto done;
    send_json(ws, "finish-task", false);
    b = xEventGroupWaitBits(s_ev, EV_FINISHED | EV_FAILED | EV_CANCEL, pdFALSE, pdFALSE, pdMS_TO_TICKS(10000));
    ok = b & EV_FINISHED;
    if (!ok) set_err("语音识别超时");
    ESP_LOGI(TAG, "finished %s, %d ms after start", ok ? "ok" : "badly", (int)(esp_log_timestamp() - t0));
done:
    if (!ok && !(xEventGroupGetBits(s_ev) & EV_CANCEL)) {
        ESP_LOGW(TAG, "recognition failed (%s)", s_err[0] ? s_err : "unknown");
    }
    mic_stop();
    /* Advance generation before async destroy so late close events never hit a new session. */
    if (ws) {
        if (s_gen == gen) s_gen++;
        destroy_ws_async(ws);
        ws = NULL;
    }
    if (headers) {
        memset(headers, 0, 320);
        free(headers);
    }
    if (key) {
        memset(key, 0, 256);
        free(key);
    }
    free(chunk);
    net_awake(false);
    /* Fire callback right away (socket closes asynchronously), then release EV_IDLE. */
    bool cancelled = (xEventGroupGetBits(s_ev) & EV_CANCEL) != 0;
    asr_text_cb_t cb = s_cb;
    if (!cancelled && cb) {
        cb(ok ? s_text : NULL, true);   /* NULL: failed */
    }
    xEventGroupSetBits(s_ev, EV_IDLE);
    vTaskDelete(NULL);
}

bool asr_begin(asr_text_cb_t cb) { return asr_begin_lang(cb, "\"zh\",\"en\""); }

bool asr_begin_lang(asr_text_cb_t cb, const char *langs)
{
    s_langs = langs;
    if (!s_ev) {
        s_ev = xEventGroupCreate();
        if (!s_ev) return false;
        xEventGroupSetBits(s_ev, EV_IDLE);
        static StaticStreamBuffer_t sb;
        uint8_t *storage = heap_caps_malloc(AUDIO_BUF + 1, MALLOC_CAP_SPIRAM);
        if (storage) s_audio = xStreamBufferCreateStatic(AUDIO_BUF, 1, storage, &sb);
        s_done = heap_caps_calloc(1, TEXT_CAP, MALLOC_CAP_SPIRAM);
        s_text = heap_caps_calloc(1, TEXT_CAP, MALLOC_CAP_SPIRAM);
        s_msg = heap_caps_malloc(MSG_CAP, MALLOC_CAP_SPIRAM);
    }
    if (!s_audio || !s_done || !s_text || !s_msg) return false;
    if (!(xEventGroupWaitBits(s_ev, EV_IDLE, pdFALSE, pdTRUE, 0) & EV_IDLE)) {
        return false;   /* the last one is still winding down */
    }
    xEventGroupClearBits(s_ev, 0xff);
    xStreamBufferReset(s_audio);
    s_done[0] = s_text[0] = '\0';
    s_err[0] = '\0';
    s_cb = cb;
    uint32_t gen = ++s_gen;
    for (int i = 0; i < 16; i++) sprintf(&s_task_id[2 * i], "%02x", (unsigned)(esp_random() & 0xff));
    mic_start(audio_sink);
    net_awake(true);
    if (xTaskCreatePinnedToCore(asr_task, "asr", 4096, (void *)(uintptr_t)gen, 5, NULL, 0) != pdPASS) {
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

bool asr_active(void)
{
    return s_ev && !(xEventGroupGetBits(s_ev) & EV_IDLE);
}

const char *asr_last_error(void)
{
    return s_err;
}
