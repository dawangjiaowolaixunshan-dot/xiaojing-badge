#include "chat.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "history.h"
#include "net.h"
#include "voices.h"
#include "weather.h"
#include "settings.h"

static const char *TAG = "chat";
#define URL "https://api.deepseek.com/chat/completions"
#define MODEL "deepseek-flash"   /* "deepseek-chat" was retired 2026-07-24 */
#define HISTORY 10          /* exchanges kept as context, in full */
#define REPLY_CAP 4096
#define TURN_CAP 1024
#define LINE_CAP 32768   /* one SSE line; the last one lists the search sources */

/* Without a prompt of the user's own (tools/badge_setup.py prompt <file>). */
static const char *DEFAULT_PROMPT = "你在一个挂在胸前的小圆屏吧唧里，屏幕很小，只能显示文字。";
/* Always last: what the small screen can show. */
static const char *FORMAT_RULES =
    "\n\n输出要求：回答会被念出来，同时显示在小屏上。用简体中文口语；"
    "简单的问题一两句话说完，复杂的问题可以讲清楚，但最多300字。"
    "不要用Markdown、标题、列表符号、表格和表情符号，不要换行分段。"
    "如果上网搜索了，用自己的话简单概括，不要念网址、来源编号，也不要大段照搬原文。"
    "\n\n家人：问题后面会注明说话的是谁。孩子是你的小主人；如果是爸爸或妈妈，就称呼“爸爸”“妈妈”，"
    "用跟大人说话的自然口吻，可以说得更直接，但其他规则照样遵守。没有注明时，按孩子对待。";

typedef struct {
    char *q, *a;
    voice_who_t who;   /* who asked q */
} turn_t;

static turn_t s_hist[HISTORY];
static int s_nhist;
static voice_who_t s_who = VOICE_UNKNOWN;   /* who asked this question */

static volatile bool s_busy;
static bool s_filtered;   /* DeepSeek's review stopped the answer */
static int s_searches;
static chat_search_cb_t s_on_search;
static char *s_question, *s_reply, *s_line;
static chat_text_cb_t s_on_text;
static chat_done_cb_t s_on_done;

static char *psram_strdup(const char *s, size_t cap)
{
    size_t n = strnlen(s, cap - 1);
    char *d = heap_caps_malloc(n + 1, MALLOC_CAP_SPIRAM);
    if (d) {
        memcpy(d, s, n);
        d[n] = '\0';
    }
    return d;
}

/* A copy of s with anything that isn't valid UTF-8 dropped. One bad byte
 * (a cut in the middle of a character, in this request or in an old chat
 * that's recalled) and DeepSeek refuses the whole request with a 400. */
static char *utf8_clean(const char *s)
{
    size_t n = strlen(s);
    char *d = heap_caps_malloc(n + 1, MALLOC_CAP_SPIRAM);
    if (!d) return NULL;
    const unsigned char *p = (const unsigned char *)s;
    size_t o = 0;
    while (*p) {
        unsigned c = *p;
        int len = c < 0x80 ? 1 : (c >= 0xC2 && c <= 0xDF) ? 2 : (c >= 0xE0 && c <= 0xEF) ? 3 : (c >= 0xF0 && c <= 0xF4) ? 4 : 0;
        bool ok = len > 0;
        for (int i = 1; ok && i < len; i++) ok = (p[i] & 0xC0) == 0x80;
        if (ok && len == 3) {
            unsigned cp = (c & 0x0F) << 12 | (p[1] & 0x3F) << 6 | (p[2] & 0x3F);
            ok = cp >= 0x800 && (cp < 0xD800 || cp > 0xDFFF);   /* no overlongs, no surrogates */
        } else if (ok && len == 4) {
            unsigned cp = (c & 0x07) << 18 | (p[1] & 0x3F) << 12 | (p[2] & 0x3F) << 6 | (p[3] & 0x3F);
            ok = cp >= 0x10000 && cp <= 0x10FFFF;
        }
        if (!ok) {
            p++;   /* skip the bad byte */
            continue;
        }
        if (len == 1 && c < 0x20 && c != '\n' && c != '\t') {   /* stray control bytes too */
            p++;
            continue;
        }
        memcpy(d + o, p, len);
        o += len;
        p += len;
    }
    d[o] = '\0';
    if (o != n) ESP_LOGW(TAG, "dropped %d bad bytes from a message", (int)(n - o));
    return d;
}

static void add_msg(cJSON *msgs, const char *role, const char *content);

/* A question with who asked it after it, as the prompt's 家人 rule says;
 * past ones too, so "爸爸 asked, then the child" doesn't run together. */
static void add_question(cJSON *msgs, const char *q, voice_who_t who)
{
    if (who == VOICE_UNKNOWN) {
        add_msg(msgs, "user", q);
        return;
    }
    size_t n = strlen(q) + 48;
    char *t = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
    if (!t) {
        add_msg(msgs, "user", q);
        return;
    }
    snprintf(t, n, "%s\n（说话的是：%s）", q, voices_name(who));
    add_msg(msgs, "user", t);
    free(t);
}

static void add_msg(cJSON *msgs, const char *role, const char *content)
{
    char *clean = utf8_clean(content);
    cJSON *m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "role", role);
    cJSON_AddStringToObject(m, "content", clean ? clean : "");
    cJSON_AddItemToArray(msgs, m);
    free(clean);
}

static char *build_body(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "model", MODEL);
    cJSON_AddBoolToObject(root, "stream", true);
    cJSON_AddNumberToObject(root, "max_tokens", 800);
    /* deepseek-flash thinks by default: slow to the first word, and a
     * spoken answer gains nothing from it. */
    cJSON_AddStringToObject(cJSON_AddObjectToObject(root, "thinking"), "type", "disabled");
    /* DeepSeek's own content review on top of the prompt: a child uses this. */
    cJSON_AddStringToObject(cJSON_AddObjectToObject(root, "safety_check"), "level", "high");
    /* DeepSeek's own web search: the model searches when a question needs
     * something newer than it knows (news, weather, scores). */
    cJSON *tool = cJSON_CreateObject();
    cJSON_AddStringToObject(tool, "type", "deepseek_web_search");
    cJSON_AddItemToArray(cJSON_AddArrayToObject(root, "tools"), tool);
    cJSON *msgs = cJSON_AddArrayToObject(root, "messages");
    char *custom = prompt_load();
    const char *base = custom ? custom : DEFAULT_PROMPT;
    size_t n = strlen(base) + strlen(FORMAT_RULES) + 1;
    char *system = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
    if (system) {
        snprintf(system, n, "%s%s", base, FORMAT_RULES);
        add_msg(msgs, "system", system);
        free(system);
    }
    free(custom);
    for (int i = 0; i < s_nhist; i++) {
        add_question(msgs, s_hist[i].q, s_hist[i].who);
        add_msg(msgs, "assistant", s_hist[i].a);
    }
    /* The model can't know the date; the badge's clock (from NTP) can. It
     * goes on this question only, so the prompt and the past exchanges stay
     * the same from one request to the next and hit DeepSeek's cache. */
    if (net_time_valid()) {
        static const char *WEEK[] = {"日", "一", "二", "三", "四", "五", "六"};
        time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        char wx[160];
        weather_line(wx, sizeof(wx));   /* empty until the weather's in */
        /* Older talk on the same thing, beyond the 10 exchanges sent anyway:
         * so "the whale you told me about" still finds the whale. */
        char *recall = heap_caps_malloc(1600, MALLOC_CAP_SPIRAM);
        if (recall) history_recall(s_question, s_nhist, recall, 1600);
        size_t qn = strlen(s_question) + 640 + (recall ? strlen(recall) + 256 : 0);   /* the Chinese wrapping alone is ~100 bytes each; too small and the last "）" is cut in half (400) */
        char *q = heap_caps_malloc(qn, MALLOC_CAP_SPIRAM);
        if (q) {
            int at = snprintf(q, qn, "%s\n（背景信息，只在问到时间、日期、天气、穿衣或出门时才用，平时不要提：%s%s%s现在是北京时间 %d年%d月%d日 星期%s %02d:%02d%s%s）", s_question,
                     s_who != VOICE_UNKNOWN ? "说话的是：" : "", s_who != VOICE_UNKNOWN ? voices_name(s_who) : "",
                     s_who != VOICE_UNKNOWN ? "；" : "",
                     tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, WEEK[tm.tm_wday], tm.tm_hour, tm.tm_min,
                     wx[0] ? "；天气 " : "", wx);
            if (recall && recall[0] && at > 0 && (size_t)at < qn) {
                snprintf(q + at, qn - at, "\n（以前聊过的相关内容，只在这次问题用得上时参考，不要主动复述：\n%s）", recall);
            }
            add_msg(msgs, "user", q);
            free(q);
            free(recall);
        } else {
            free(recall);
            add_question(msgs, s_question, s_who);
        }
    } else {
        add_question(msgs, s_question, s_who);   /* no clock yet: still say who's asking */
    }
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return body;
}

/* One SSE line: "data: {...}" with choices[0].delta.content. */
static void handle_line(const char *line, size_t *len)
{
    if (strncmp(line, "data: ", 6) || !strcmp(line + 6, "[DONE]")) return;
    cJSON *root = cJSON_Parse(line + 6);
    cJSON *choice = cJSON_GetArrayItem(cJSON_GetObjectItem(root, "choices"), 0);
    const char *ev = cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetObjectItem(choice, "server_tool_event"), "type"));
    if (ev && !strcmp(ev, "call_started")) {
        s_searches++;
        if (s_on_search) s_on_search();
    }
    const char *finish = cJSON_GetStringValue(cJSON_GetObjectItem(choice, "finish_reason"));
    if (finish && !strcmp(finish, "content_filter")) s_filtered = true;
    const char *piece = cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetObjectItem(choice, "delta"), "content"));
    if (piece && *piece) {
        size_t n = strlen(piece);
        if (*len + n < REPLY_CAP) {
            memcpy(s_reply + *len, piece, n);
            *len += n;
            s_reply[*len] = '\0';
            if (s_on_text) s_on_text(s_reply);
        }
    }
    cJSON_Delete(root);
}

static void remember(void)
{
    if (s_nhist == HISTORY) {
        free(s_hist[0].q);
        free(s_hist[0].a);
        memmove(&s_hist[0], &s_hist[1], (HISTORY - 1) * sizeof(turn_t));
        s_nhist--;
    }
    s_hist[s_nhist].q = psram_strdup(s_question, TURN_CAP);
    s_hist[s_nhist].a = psram_strdup(s_reply, REPLY_CAP);
    s_hist[s_nhist].who = s_who;
    if (s_hist[s_nhist].q && s_hist[s_nhist].a) s_nhist++;
}

static void forget_all(void)
{
    for (int i = 0; i < s_nhist; i++) {
        free(s_hist[i].q);
        free(s_hist[i].a);
    }
    s_nhist = 0;
}

static void restore_one(const char *q, const char *a, void *ctx)
{
    if (!q) {   /* a new conversation began here */
        forget_all();
        return;
    }
    strlcpy(s_question, q, TURN_CAP);
    strlcpy(s_reply, a, REPLY_CAP);
    s_who = voices_from_key(history_each_who());
    remember();
    s_who = VOICE_UNKNOWN;
}

void chat_restore(void)
{
    if (!s_reply) {
        s_reply = heap_caps_calloc(1, REPLY_CAP, MALLOC_CAP_SPIRAM);
        s_question = heap_caps_calloc(1, TURN_CAP, MALLOC_CAP_SPIRAM);
        s_line = heap_caps_malloc(LINE_CAP, MALLOC_CAP_SPIRAM);
    }
    history_each(HISTORY_SHOW, restore_one, NULL);
    s_reply[0] = s_question[0] = '\0';
    ESP_LOGI(TAG, "%d exchanges of context restored", s_nhist);
}

static void chat_task(void *arg)
{
    const char *error = NULL;
    static char err_buf[48];
    char *auth = heap_caps_malloc(300, MALLOC_CAP_SPIRAM);
    char *key = heap_caps_malloc(256, MALLOC_CAP_SPIRAM);
    char *body = build_body();
    char *buf = heap_caps_malloc(1024, MALLOC_CAP_SPIRAM);
    size_t reply_len = 0, line_len = 0;
    s_reply[0] = '\0';
    s_filtered = false;
    s_searches = 0;
    settings_get_key(KEY_DEEPSEEK, key, 256);
    snprintf(auth, 300, "Bearer %s", key);
    memset(key, 0, 256);

    esp_http_client_config_t cfg = {
        .url = URL,
        .method = HTTP_METHOD_POST,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 30000,
        .buffer_size = 2048,
        .buffer_size_tx = 1024,
    };
    esp_http_client_handle_t http = esp_http_client_init(&cfg);
    esp_http_client_set_header(http, "Authorization", auth);
    esp_http_client_set_header(http, "Content-Type", "application/json");
    esp_http_client_set_header(http, "Accept", "text/event-stream");
    int64_t t0 = esp_log_timestamp();
    int body_len = strlen(body);
    if (esp_http_client_open(http, body_len) != ESP_OK ||
        esp_http_client_write(http, body, body_len) != body_len) {
        error = "连不上 DeepSeek";
        goto done;
    }
    esp_http_client_fetch_headers(http);
    int status = esp_http_client_get_status_code(http);
    if (status != 200) {
        int n = esp_http_client_read(http, buf, 1023);
        buf[n > 0 ? n : 0] = '\0';
        ESP_LOGE(TAG, "HTTP %d: %.200s", status, buf);
        if (status == 400 && strstr(buf, "Content Exists Risk")) {
            snprintf(err_buf, sizeof(err_buf), "这个问题不适合聊，换一个吧");
        } else {
            snprintf(err_buf, sizeof(err_buf), status == 401 ? "DeepSeek 密钥不对 (401)" :
                     status == 402 ? "DeepSeek 余额不足 (402)" : "DeepSeek 出错了 (%d)", status);
        }
        error = err_buf;
        goto done;
    }
    bool first = true;
    for (;;) {
        int n = esp_http_client_read(http, buf, 1024);
        if (n <= 0) break;
        if (first) {
            ESP_LOGI(TAG, "first bytes after %d ms", (int)(esp_log_timestamp() - t0));
            first = false;
        }
        for (int i = 0; i < n; i++) {
            char c = buf[i];
            if (c == '\n') {
                s_line[line_len] = '\0';
                handle_line(s_line, &reply_len);
                line_len = 0;
            } else if (c != '\r' && line_len < LINE_CAP - 1) {
                s_line[line_len++] = c;
            }
        }
    }
    ESP_LOGI(TAG, "reply %d bytes in %d ms, %d search%s", (int)reply_len, (int)(esp_log_timestamp() - t0), s_searches,
             s_searches == 1 ? "" : "es");
    if (s_filtered) {
        error = "这个问题不适合聊，换一个吧";
    } else if (!reply_len) {
        error = "DeepSeek 没有回答";
    } else {
        remember();
        history_add(s_question, s_reply, voices_key(s_who));
    }
done:
    esp_http_client_cleanup(http);
    memset(auth, 0, 300);
    free(auth);
    free(key);
    free(buf);
    cJSON_free(body);
    s_busy = false;
    if (s_on_done) s_on_done(error);
    vTaskDelete(NULL);
}

bool chat_ask(const char *question, chat_text_cb_t on_text, chat_done_cb_t on_done)
{
    if (s_busy) return false;
    if (!s_reply) {
        s_reply = heap_caps_calloc(1, REPLY_CAP, MALLOC_CAP_SPIRAM);
        s_question = heap_caps_calloc(1, TURN_CAP, MALLOC_CAP_SPIRAM);
        s_line = heap_caps_malloc(LINE_CAP, MALLOC_CAP_SPIRAM);
    }
    strlcpy(s_question, question, TURN_CAP);
    s_on_text = on_text;
    s_on_done = on_done;
    s_busy = true;
    if (xTaskCreatePinnedToCore(chat_task, "chat", 8192, NULL, 5, NULL, 0) != pdPASS) {
        s_busy = false;
        return false;
    }
    return true;
}

void chat_reset(void)
{
    if (s_busy) return;
    forget_all();
    history_divider();
}

bool chat_busy(void) { return s_busy; }

void chat_on_search(chat_search_cb_t cb) { s_on_search = cb; }

void chat_set_speaker(int who) { s_who = (voice_who_t)who; }
