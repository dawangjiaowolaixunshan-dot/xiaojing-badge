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
#include "net.h"
#include "settings.h"

static const char *TAG = "tts";
#define COSY_URI "wss://dashscope.aliyuncs.com/api-ws/v1/inference"
#define COSY_MODEL "cosyvoice-v2"
#define Q3_INSTRUCT_URI "wss://dashscope.aliyuncs.com/api-ws/v1/realtime?model=qwen3-tts-instruct-flash-realtime-2026-01-22"
#define Q3_FLASH_URI "wss://dashscope.aliyuncs.com/api-ws/v1/realtime?model=qwen3-tts-flash-realtime"

#define AUDIO_BUF (TTS_RATE * 2 * 16)   /* 16 s of 24 kHz PCM in PSRAM */
#define TEXT_CAP 8192
#define MSG_CAP 65536                   /* Qwen3 audio delta JSON can be 16-48 KB */
#define PCM_DEC_CAP 49152
#define BLOCK 480                       /* 20 ms at 24 kHz */

#define EV_STARTED  BIT0
#define EV_FINISHED BIT1
#define EV_FAILED   BIT2
#define EV_NO_MORE  BIT3
#define EV_STOP     BIT4
#define EV_IDLE     BIT5
#define EV_CREATED  BIT6

static StreamBufferHandle_t s_audio;
static EventGroupHandle_t s_ev;
static SemaphoreHandle_t s_mu;
static char *s_text, *s_msg;
static uint8_t *s_pcm_dec;
static size_t s_text_len, s_msg_len;
static char s_task_id[33];
static bool s_enabled = true;
static volatile bool s_flush;
static volatile bool s_is_q3;
static uint32_t s_evt_seq;

/* Sentence tracking for CosyVoice poem line-by-line sync. */
#define MAX_SENTENCES 48
static volatile uint32_t s_rx_bytes, s_played;
static volatile int32_t s_sent_at[MAX_SENTENCES];
static volatile int s_nsent;
static volatile int s_level;
static const char *s_voice = "longhua_v2";
static const char *s_task_voice;

/* ---- the speaker (24 kHz mono -> 48 kHz stereo with soft clarity boost) ---- */

static size_t prime_bytes(void)
{
    /* Weak signal needs a deeper jitter buffer so playback stays smooth instead of stuttering. */
    int ms = net_rssi() < -72 ? 850 : 550;
    return (size_t)(TTS_RATE * 2 * ms / 1000);
}

static size_t reprime_bytes(void)
{
    int ms = net_rssi() < -72 ? 650 : 400;
    return (size_t)(TTS_RATE * 2 * ms / 1000);
}

static volatile int s_underruns;

static bool synth_over(void) { return (xEventGroupGetBits(s_ev) & (EV_FINISHED | EV_FAILED | EV_STOP | EV_IDLE)) != 0; }

static inline int16_t soft_boost(int32_t x)
{
    /* +2.6 dB presence boost with gentle soft-limiting above ~26000. */
    int32_t y = (x * 135) / 100;
    if (y > 26000) y = 26000 + (y - 26000) / 3;
    else if (y < -26000) y = -26000 + (y + 26000) / 3;
    if (y > 32767) y = 32767;
    else if (y < -32768) y = -32768;
    return (int16_t)y;
}

static void player_task(void *arg)
{
    int16_t *mono = heap_caps_malloc(BLOCK * sizeof(int16_t), MALLOC_CAP_INTERNAL);
    /* 2x upsampled 24 kHz -> 48 kHz, stereo */
    int16_t *stereo = heap_caps_malloc(BLOCK * 2 * 2 * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    esp_codec_dev_handle_t spk = mic_speaker();
    size_t need = prime_bytes();
    int16_t prev = 0;
    for (;;) {
        if (s_flush) {
            while (xStreamBufferReceive(s_audio, mono, BLOCK * sizeof(int16_t), 0) > 0) {}
            need = prime_bytes();
            prev = 0;
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        if (need) {
            if (xStreamBufferBytesAvailable(s_audio) < need && !synth_over()) {
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
            need = 0;
        }
        size_t n = xStreamBufferReceive(s_audio, mono, BLOCK * sizeof(int16_t), pdMS_TO_TICKS(60));
        if (s_flush) continue;
        if (!n) {
            if (!synth_over()) {
                s_underruns++;
                need = reprime_bytes();
            } else {
                need = prime_bytes();
                prev = 0;
            }
            continue;
        }
        s_played += n;
        size_t samples = n / 2;
        int64_t sq = 0;
        for (size_t i = 0; i < samples; i++) {
            int16_t cur = soft_boost(mono[i]);
            int16_t mid = (int16_t)(((int32_t)prev + (int32_t)cur) / 2);
            /* Two 48 kHz stereo frames per 24 kHz mono sample. */
            stereo[4 * i]     = mid;
            stereo[4 * i + 1] = mid;
            stereo[4 * i + 2] = cur;
            stereo[4 * i + 3] = cur;
            prev = cur;
            sq += (int32_t)cur * cur;
        }
        int rms = samples ? (int)sqrtf((float)sq / samples) : 0;
        int lvl = rms < 200 ? 0 : (int)(28.0f * log10f(rms / 200.0f));
        s_level = lvl > 100 ? 100 : lvl;
        if (spk && samples) esp_codec_dev_write(spk, stereo, samples * 2 * 4);
    }
}

/* ---- base64 decoder for Qwen3-TTS response.audio.delta ---- */

static size_t b64_decode(const char *in, uint8_t *out, size_t cap)
{
    static const int8_t T[128] = {
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
        52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
        -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
        15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
        -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
        41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
    };
    size_t o = 0;
    uint32_t acc = 0;
    int bits = 0;
    for (; *in && *in != '='; in++) {
        unsigned char c = (unsigned char)*in;
        if (c >= 128) continue;
        int8_t v = T[c];
        if (v < 0) continue;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (o < cap) out[o++] = (uint8_t)((acc >> bits) & 0xff);
        }
    }
    return o;
}

/* ---- WebSocket event handling (CosyVoice-v2 + Qwen3-TTS-Instruct) ---- */

static void handle_cosy_msg(const char *json)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) return;
    cJSON *header = cJSON_GetObjectItem(root, "header");
    const char *event = cJSON_GetStringValue(cJSON_GetObjectItem(header, "event"));
    if (event && !strcmp(event, "result-generated")) {
        cJSON *out = cJSON_GetObjectItem(cJSON_GetObjectItem(root, "payload"), "output");
        const char *type = cJSON_GetStringValue(cJSON_GetObjectItem(out, "type"));
        if (type && !strcmp(type, "sentence-begin") && s_nsent < MAX_SENTENCES) {
            s_sent_at[s_nsent] = (int32_t)s_rx_bytes;
            s_nsent++;
        }
    }
    if (event && !strcmp(event, "task-started")) xEventGroupSetBits(s_ev, EV_STARTED);
    else if (event && !strcmp(event, "task-finished")) xEventGroupSetBits(s_ev, EV_FINISHED);
    else if (event && !strcmp(event, "task-failed")) {
        ESP_LOGE(TAG, "cosy task failed: %s %s",
                 cJSON_GetStringValue(cJSON_GetObjectItem(header, "error_code")) ?: "",
                 cJSON_GetStringValue(cJSON_GetObjectItem(header, "error_message")) ?: "");
        xEventGroupSetBits(s_ev, EV_FAILED);
    }
    cJSON_Delete(root);
}

static void handle_q3_msg(const char *json)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) return;
    const char *type = cJSON_GetStringValue(cJSON_GetObjectItem(root, "type"));
    if (!type) {
        cJSON_Delete(root);
        return;
    }
    if (!strcmp(type, "session.created")) {
        xEventGroupSetBits(s_ev, EV_CREATED);
    } else if (!strcmp(type, "session.updated")) {
        xEventGroupSetBits(s_ev, EV_STARTED);
    } else if (!strcmp(type, "response.audio.delta")) {
        const char *delta = cJSON_GetStringValue(cJSON_GetObjectItem(root, "delta"));
        if (delta && *delta && s_pcm_dec && !(xEventGroupGetBits(s_ev) & EV_STOP)) {
            size_t n = b64_decode(delta, s_pcm_dec, PCM_DEC_CAP);
            if (n > 0) {
                xStreamBufferSend(s_audio, s_pcm_dec, n, pdMS_TO_TICKS(2000));
                s_rx_bytes += n;
            }
        }
    } else if (!strcmp(type, "session.finished")) {
        xEventGroupSetBits(s_ev, EV_FINISHED);
    } else if (!strcmp(type, "error")) {
        cJSON *err = cJSON_GetObjectItem(root, "error");
        ESP_LOGE(TAG, "qwen3 error: %s %s",
                 cJSON_GetStringValue(cJSON_GetObjectItem(err, "code")) ?: "",
                 cJSON_GetStringValue(cJSON_GetObjectItem(err, "message")) ?: "");
        xEventGroupSetBits(s_ev, EV_FAILED);
    } else {
        ESP_LOGI(TAG, "qwen3 event: %s", type);
    }
    cJSON_Delete(root);
}

static void ws_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    esp_websocket_event_data_t *d = data;
    if (id == WEBSOCKET_EVENT_CONNECTED) {
        ESP_LOGI(TAG, "ws connected");
    } else if (id == WEBSOCKET_EVENT_DATA && d->op_code == 0x2) {   /* CosyVoice binary PCM */
        if (!(xEventGroupGetBits(s_ev) & EV_STOP) && d->data_len > 0) {
            xStreamBufferSend(s_audio, d->data_ptr, d->data_len, pdMS_TO_TICKS(2000));
            s_rx_bytes += d->data_len;
        }
    } else if (id == WEBSOCKET_EVENT_DATA && (d->op_code == 0x1 || d->op_code == 0x0)) {
        if (d->op_code == 0x1 && d->payload_offset == 0) s_msg_len = 0;
        if (s_msg && s_msg_len + d->data_len < MSG_CAP) {
            memcpy(s_msg + s_msg_len, d->data_ptr, d->data_len);
            s_msg_len += d->data_len;
        }
        if (d->payload_offset + d->data_len < d->payload_len) return;
        if (s_msg) {
            s_msg[s_msg_len] = '\0';
            if (s_is_q3) handle_q3_msg(s_msg);
            else handle_cosy_msg(s_msg);
        }
        s_msg_len = 0;
    } else if (id == WEBSOCKET_EVENT_DATA && d->op_code == 0x8) {
        int code = d->data_len >= 2 ? (((uint8_t)d->data_ptr[0] << 8) | (uint8_t)d->data_ptr[1]) : 0;
        ESP_LOGW(TAG, "ws close %d: %.*s", code, d->data_len > 2 ? d->data_len - 2 : 0,
                 d->data_len > 2 ? d->data_ptr + 2 : "");
        if (!(xEventGroupGetBits(s_ev) & EV_FINISHED)) xEventGroupSetBits(s_ev, EV_FAILED);
    } else if (id == WEBSOCKET_EVENT_DISCONNECTED || id == WEBSOCKET_EVENT_ERROR) {
        ESP_LOGW(TAG, "ws ev=%d http=%d err=%d", (int)id, d ? d->error_handle.esp_ws_handshake_status_code : -1,
                 d ? (int)d->error_handle.error_type : -1);
        if (!(xEventGroupGetBits(s_ev) & EV_FINISHED)) xEventGroupSetBits(s_ev, EV_FAILED);
    }
}

/* ---- CosyVoice commands ---- */

static void send_cosy_cmd(esp_websocket_client_handle_t ws, const char *action, const char *text)
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
        cJSON_AddStringToObject(payload, "model", COSY_MODEL);
        cJSON *p = cJSON_AddObjectToObject(payload, "parameters");
        cJSON_AddStringToObject(p, "text_type", "PlainText");
        cJSON_AddStringToObject(p, "voice", s_task_voice ? s_task_voice : s_voice);
        cJSON_AddStringToObject(p, "format", "pcm");
        cJSON_AddNumberToObject(p, "sample_rate", TTS_RATE);
        cJSON_AddNumberToObject(p, "volume", 75);
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

/* ---- Qwen3-TTS-Instruct Realtime commands ---- */

static void send_q3_json(esp_websocket_client_handle_t ws, cJSON *root)
{
    char eid[24];
    snprintf(eid, sizeof(eid), "e_%u", (unsigned)++s_evt_seq);
    cJSON_AddStringToObject(root, "event_id", eid);
    char *msg = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (msg) {
        esp_websocket_client_send_text(ws, msg, strlen(msg), pdMS_TO_TICKS(3000));
        cJSON_free(msg);
    }
}

static void send_q3_session(esp_websocket_client_handle_t ws, const char *voice, const char *instr)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "session.update");
    cJSON *sess = cJSON_AddObjectToObject(root, "session");
    cJSON_AddStringToObject(sess, "voice", voice);
    cJSON_AddStringToObject(sess, "mode", "server_commit");
    cJSON_AddStringToObject(sess, "language_type", "Chinese");
    cJSON_AddStringToObject(sess, "response_format", "pcm");
    cJSON_AddNumberToObject(sess, "sample_rate", TTS_RATE);
    if (instr && *instr) {
        cJSON_AddStringToObject(sess, "instructions", instr);
    }
    send_q3_json(ws, root);
}

static void send_q3_append(esp_websocket_client_handle_t ws, const char *text)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "input_text_buffer.append");
    cJSON_AddStringToObject(root, "text", text);
    send_q3_json(ws, root);
}

static void send_q3_finish(esp_websocket_client_handle_t ws)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "session.finish");
    send_q3_json(ws, root);
}

/* ---- Synthesis task ---- */

static void tts_task(void *arg)
{
    char *key = heap_caps_calloc(1, 256, MALLOC_CAP_SPIRAM);
    char *headers = heap_caps_calloc(1, 320, MALLOC_CAP_SPIRAM);
    char *piece = heap_caps_calloc(1, TEXT_CAP, MALLOC_CAP_SPIRAM);
    if (!key || !headers || !piece || !settings_get_key(KEY_BAILIAN, key, 256)) {
        ESP_LOGW(TAG, "missing key or memory");
        goto cleanup_early;
    }

    const char *spec = s_task_voice ? s_task_voice : s_voice;
    bool is_q3 = !strncmp(spec, "q3:", 3);
    s_is_q3 = is_q3;

    char q3_voice[48] = "";
    const char *q3_instr = NULL;
    if (is_q3) {
        const char *p = spec + 3;
        const char *bar = strchr(p, '|');
        size_t vn = bar ? (size_t)(bar - p) : strlen(p);
        if (vn >= sizeof(q3_voice)) vn = sizeof(q3_voice) - 1;
        memcpy(q3_voice, p, vn);
        q3_voice[vn] = '\0';
        q3_instr = (bar && bar[1]) ? bar + 1 : NULL;
        snprintf(headers, 320, "Authorization: Bearer %s\r\n", key);
    } else {
        snprintf(headers, 320, "Authorization: bearer %s\r\nX-DashScope-DataInspection: enable\r\n", key);
    }
    memset(key, 0, 256);

    esp_websocket_client_handle_t ws = NULL;
    int64_t t0 = esp_log_timestamp();
    bool sent_any = false;
    const char *why = "connect";
    int tries = (is_q3 && q3_instr) ? 2 : 1;
    bool started = false;
    for (int attempt = 0; attempt < tries && !started; attempt++) {
        if (ws) {
            esp_websocket_client_destroy(ws);
            ws = NULL;
        }
        bool use_instr = is_q3 && q3_instr && attempt == 0;
        const char *try_uri = !is_q3 ? COSY_URI : (use_instr ? Q3_INSTRUCT_URI : Q3_FLASH_URI);
        if (attempt > 0) {
            ESP_LOGW(TAG, "qwen3-instruct unavailable, falling back to qwen3-flash");
        }
        xEventGroupClearBits(s_ev, EV_CREATED | EV_STARTED | EV_FAILED | EV_FINISHED);
        esp_websocket_client_config_t cfg = {
            .uri = try_uri,
            .headers = headers,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .buffer_size = 4096,
            .task_stack = 6144,
            .network_timeout_ms = 10000,
            .disable_auto_reconnect = true,
        };
        ws = esp_websocket_client_init(&cfg);
        if (!ws) continue;
        esp_websocket_register_events(ws, WEBSOCKET_EVENT_ANY, ws_event, NULL);
        why = "connect";
        if (esp_websocket_client_start(ws) != ESP_OK) {
            ESP_LOGW(TAG, "ws start failed (int free=%u max=%u)",
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
            continue;
        }
        for (int i = 0; i < 100 && !esp_websocket_client_is_connected(ws); i++) {
            if (xEventGroupGetBits(s_ev) & (EV_STOP | EV_FAILED)) break;
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        if (!esp_websocket_client_is_connected(ws) || (xEventGroupGetBits(s_ev) & EV_STOP)) continue;

        why = "start";
        if (is_q3) {
            xEventGroupWaitBits(s_ev, EV_CREATED | EV_FAILED | EV_STOP, pdFALSE, pdFALSE, pdMS_TO_TICKS(500));
            if (xEventGroupGetBits(s_ev) & (EV_FAILED | EV_STOP)) continue;
            send_q3_session(ws, q3_voice, use_instr ? q3_instr : NULL);
        } else {
            send_cosy_cmd(ws, "run-task", NULL);
        }
        if (xEventGroupWaitBits(s_ev, EV_STARTED | EV_FAILED | EV_STOP, pdFALSE, pdFALSE, pdMS_TO_TICKS(5000)) & EV_STARTED) {
            started = true;
        }
    }
    if (!started) goto fail;
    ESP_LOGI(TAG, "%s started (%s) in %d ms", is_q3 ? "qwen3" : "cosy", is_q3 ? q3_voice : spec,
             (int)(esp_log_timestamp() - t0));

    for (;;) {
        EventBits_t b = xEventGroupGetBits(s_ev);
        if (b & (EV_STOP | EV_FAILED)) goto done;
        xSemaphoreTake(s_mu, portMAX_DELAY);
        size_t n = s_text_len;
        memcpy(piece, s_text, n);
        piece[n] = '\0';
        s_text_len = 0;
        xSemaphoreGive(s_mu);
        if (n) {
            for (char *seg = piece, *next; seg && *seg; seg = next) {
                next = strchr(seg, '\x1e');
                if (next) *next++ = '\0';
                if (!*seg) continue;
                if (is_q3) send_q3_append(ws, seg);
                else send_cosy_cmd(ws, "continue-task", seg);
            }
            sent_any = true;
        } else if (b & EV_NO_MORE) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(150));
    }
    if (!sent_any) goto done;
    if (is_q3) send_q3_finish(ws);
    else send_cosy_cmd(ws, "finish-task", NULL);
    xEventGroupWaitBits(s_ev, EV_FINISHED | EV_FAILED | EV_STOP, pdFALSE, pdFALSE, pdMS_TO_TICKS(60000));
    ESP_LOGI(TAG, "done %d ms after start, %u bytes, %d underruns",
             (int)(esp_log_timestamp() - t0), (unsigned)s_rx_bytes, s_underruns);
    goto done;
fail:
    ESP_LOGW(TAG, "%s failed%s", why, xEventGroupGetBits(s_ev) & EV_STOP ? " (stopped)" : "");
done:
    if (ws) esp_websocket_client_destroy(ws);
cleanup_early:
    if (headers) {
        memset(headers, 0, 320);
        free(headers);
    }
    if (key) {
        memset(key, 0, 256);
        free(key);
    }
    free(piece);
    net_awake(false);
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
    s_msg = heap_caps_malloc(MSG_CAP, MALLOC_CAP_SPIRAM);
    s_pcm_dec = heap_caps_malloc(PCM_DEC_CAP, MALLOC_CAP_SPIRAM);
    uint8_t on = 1;
    if (settings_get_blob("voice", &on, 1)) s_enabled = on;
    xTaskCreatePinnedToCore(player_task, "player", 3072, NULL, 12, NULL, 1);
}

static bool begin_with(const char *voice);

bool tts_begin(void)
{
    if (!s_enabled) {
        ESP_LOGW(TAG, "not speaking: voice off");
        return false;
    }
    return begin_with(s_voice);
}

static bool begin_with(const char *voice)
{
    /* If a previous utterance is still winding down after tts_stop(), wait briefly instead of dropping speech. */
    if (!(xEventGroupGetBits(s_ev) & EV_IDLE)) {
        if (!(xEventGroupWaitBits(s_ev, EV_IDLE, pdFALSE, pdTRUE, pdMS_TO_TICKS(1800)) & EV_IDLE)) {
            ESP_LOGW(TAG, "not speaking: busy");
            return false;
        }
    }
    ESP_LOGI(TAG, "begin (%.40s)", voice);
    net_awake(true);
    s_underruns = 0;
    s_rx_bytes = s_played = 0;
    s_nsent = 0;
    s_msg_len = 0;
    for (int i = 0; i < MAX_SENTENCES; i++) s_sent_at[i] = -1;
    s_task_voice = voice;
    xEventGroupClearBits(s_ev, 0xff);
    s_flush = true;
    vTaskDelay(pdMS_TO_TICKS(30));
    s_text_len = 0;
    s_flush = false;
    for (int i = 0; i < 16; i++) sprintf(&s_task_id[2 * i], "%02x", (unsigned)(esp_random() & 0xff));
    if (xTaskCreatePinnedToCore(tts_task, "tts", 4096, NULL, 5, NULL, 0) != pdPASS) {
        net_awake(false);
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
    xEventGroupSetBits(s_ev, EV_STOP);
    s_flush = true;
}

bool tts_enabled(void) { return s_enabled; }

int tts_level(void) { return tts_active() ? s_level : 0; }

bool tts_active(void)
{
    if (!s_ev) return false;
    return !s_flush && (!(xEventGroupGetBits(s_ev) & EV_IDLE) || xStreamBufferBytesAvailable(s_audio) > 0);
}

void tts_set_enabled(bool on)
{
    s_enabled = on;
    uint8_t v = on;
    settings_set_blob("voice", &v, 1);
    if (!on) tts_stop();
}

void tts_set_voice(const char *voice) { s_voice = voice; }

typedef struct {
    char *text;
    const char *voice;
} say_t;

static void say_task(void *arg)
{
    say_t *say = arg;
    xEventGroupWaitBits(s_ev, EV_IDLE, pdFALSE, pdTRUE, pdMS_TO_TICKS(4000));
    if (begin_with(say->voice)) {
        tts_feed(say->text);
        tts_finish();
    }
    free(say->text);
    free(say);
    vTaskDelete(NULL);
}

void tts_say_with(const char *voice, const char *text)
{
    if (!s_ev) return;
    tts_stop();
    say_t *say = heap_caps_malloc(sizeof(say_t), MALLOC_CAP_SPIRAM);
    size_t n = strlen(text);
    char *copy = heap_caps_malloc(n + 1, MALLOC_CAP_SPIRAM);
    if (!say || !copy) {
        free(say);
        free(copy);
        return;
    }
    memcpy(copy, text, n + 1);
    say->text = copy;
    say->voice = voice;
    if (xTaskCreate(say_task, "say", 3072, say, 4, NULL) != pdPASS) {
        free(copy);
        free(say);
    }
}

void tts_say(const char *text) { tts_say_with(s_voice, text); }

void tts_set_volume(int pct)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    esp_codec_dev_handle_t spk = mic_speaker();
    if (!spk) return;
    if (pct <= 0) {
        esp_codec_dev_set_out_mute(spk, true);
        esp_codec_dev_set_out_vol(spk, 0);
    } else {
        esp_codec_dev_set_out_mute(spk, false);
        esp_codec_dev_set_out_vol(spk, pct);
    }
}

int tts_sentence_now(void)
{
    if (!tts_active()) return -1;
    int now = -1;
    for (int i = 0; i < s_nsent; i++) {
        if (s_sent_at[i] >= 0 && (uint32_t)s_sent_at[i] <= s_played) now = i;
    }
    return now;
}

uint32_t tts_played_ms(void)
{
    /* 24 kHz 16-bit mono = 48 bytes/ms */
    return s_played / (TTS_RATE * 2 / 1000);
}

uint32_t tts_total_ms(void)
{
    if (!s_ev) return 0;
    if (!(xEventGroupGetBits(s_ev) & EV_FINISHED)) return 0;
    return s_rx_bytes / (TTS_RATE * 2 / 1000);
}

bool tts_failed(void)
{
    return s_ev && (xEventGroupGetBits(s_ev) & EV_FAILED) != 0;
}
