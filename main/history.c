#include "history.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

void history_recall_add(const char *q, const char *a);

static const char *TAG = "history";
#define PATH "/hist/chat.jsonl"
#define TMP "/hist/chat.tmp"
#define TRIM_BYTES (400 * 1024)   /* past this, drop the oldest lines... */
#define KEEP_BYTES (250 * 1024)   /* ...down to about this: room for the copy on a 1 MB SPIFFS */
#define LINE_CAP 8192

static SemaphoreHandle_t s_mu;
static bool s_ok;

void history_init(void)
{
    s_mu = xSemaphoreCreateMutex();
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/hist",
        .partition_label = "storage",
        .max_files = 3,
        .format_if_mount_failed = true,
    };
    esp_err_t err = esp_vfs_spiffs_register(&conf);
    s_ok = err == ESP_OK;
    if (!s_ok) ESP_LOGE(TAG, "mount failed (%s): history off", esp_err_to_name(err));
    else ESP_LOGI(TAG, "mounted");
}

static void append_line(const char *line)
{
    FILE *f = fopen(PATH, "a");
    if (!f) {
        ESP_LOGE(TAG, "can't open %s for append", PATH);
        return;
    }
    int r = fputs(line, f);
    fputc('\n', f);
    if (fclose(f) || r < 0) ESP_LOGE(TAG, "write failed");
    else ESP_LOGI(TAG, "saved %d bytes", (int)strlen(line) + 1);
}

/* Each line of the file, oldest first. */
static int count_lines(void)
{
    FILE *f = fopen(PATH, "r");
    if (!f) return 0;
    int n = 0, c;
    while ((c = fgetc(f)) != EOF) n += c == '\n';
    fclose(f);
    return n;
}

static void trim(void)
{
    struct stat st;
    if (stat(PATH, &st) || st.st_size < TRIM_BYTES) return;
    long drop = st.st_size - KEEP_BYTES;   /* whole lines, oldest first, until this much is gone */
    FILE *in = fopen(PATH, "r"), *out = fopen(TMP, "w");
    char *line = heap_caps_malloc(LINE_CAP, MALLOC_CAP_SPIRAM);
    bool ok = in && out && line;
    if (ok) {
        long gone = 0;
        while (fgets(line, LINE_CAP, in)) {
            if (gone < drop) {
                gone += strlen(line);
                continue;
            }
            if (fputs(line, out) < 0) {
                ok = false;
                break;
            }
        }
    }
    if (in) fclose(in);
    if (out && fclose(out)) ok = false;
    free(line);
    if (ok) {
        remove(PATH);
        rename(TMP, PATH);
        ESP_LOGI(TAG, "trimmed from %ld bytes", (long)st.st_size);
    } else {
        remove(TMP);   /* keep the full file rather than half a copy */
        ESP_LOGE(TAG, "trim failed");
    }
}

void history_add(const char *q, const char *a, const char *who)
{
    if (!s_ok) return;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "q", q);
    cJSON_AddStringToObject(o, "a", a);
    if (who && *who) cJSON_AddStringToObject(o, "s", who);   /* child / dad / mom */
    time_t now = time(NULL);
    if (now > 1767225600) cJSON_AddNumberToObject(o, "t", (double)now);   /* once the clock is set */
    char *line = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    xSemaphoreTake(s_mu, portMAX_DELAY);
    if (line) append_line(line);
    trim();
    xSemaphoreGive(s_mu);
    cJSON_free(line);
    history_recall_add(q, a);
}

void history_divider(void)
{
    if (!s_ok) return;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    char line[48];
    time_t now = time(NULL);
    if (now > 1767225600) snprintf(line, sizeof(line), "{\"new\":1,\"t\":%lld}", (long long)now);
    else snprintf(line, sizeof(line), "{\"new\":1}");
    append_line(line);
    xSemaphoreGive(s_mu);
}

void history_each(int max, void (*fn)(const char *q, const char *a, void *ctx), void *ctx)
{
    if (!s_ok) return;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    int skip = count_lines() - max;
    FILE *f = fopen(PATH, "r");
    char *line = heap_caps_malloc(LINE_CAP, MALLOC_CAP_SPIRAM);
    if (f && line) {
        while (fgets(line, LINE_CAP, f)) {
            if (skip-- > 0) continue;
            cJSON *o = cJSON_Parse(line);
            if (!o) continue;
            const char *q = cJSON_GetStringValue(cJSON_GetObjectItem(o, "q"));
            const char *a = cJSON_GetStringValue(cJSON_GetObjectItem(o, "a"));
            if (cJSON_GetObjectItem(o, "new")) fn(NULL, NULL, ctx);
            else if (q && a) fn(q, a, ctx);
            cJSON_Delete(o);
        }
    }
    if (f) fclose(f);
    free(line);
    xSemaphoreGive(s_mu);
}

#define PROMPT_PATH "/hist/prompt.md"

char *prompt_load(void)
{
    if (!s_ok) return NULL;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    char *text = NULL;
    FILE *f = fopen(PROMPT_PATH, "r");
    if (f) {
        text = heap_caps_malloc(PROMPT_MAX + 1, MALLOC_CAP_SPIRAM);
        size_t n = text ? fread(text, 1, PROMPT_MAX, f) : 0;
        if (text) text[n] = '\0';
        if (text && !n) {
            free(text);
            text = NULL;
        }
        fclose(f);
    }
    xSemaphoreGive(s_mu);
    return text;
}

bool prompt_save(const char *text, size_t len)
{
    if (!s_ok) return false;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    bool ok;
    if (!len) {
        remove(PROMPT_PATH);
        ok = true;
    } else {
        FILE *f = fopen(PROMPT_PATH, "w");
        ok = f && fwrite(text, 1, len, f) == len;
        if (f) fclose(f);
    }
    xSemaphoreGive(s_mu);
    return ok;
}

void history_dump(void (*line_fn)(const char *line))
{
    if (!s_ok) return;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    FILE *f = fopen(PATH, "r");
    char *line = heap_caps_malloc(LINE_CAP, MALLOC_CAP_SPIRAM);
    if (f && line) {
        while (fgets(line, LINE_CAP, f)) {
            line[strcspn(line, "\n")] = '\0';
            if (line[0]) line_fn(line);
        }
    }
    if (f) fclose(f);
    free(line);
    xSemaphoreGive(s_mu);
}

/* ---- recall: the whole record in PSRAM, searched for what a question is about ---- */

#define MEM_MAX 1000
typedef struct {
    char *q, *a;
} mem_t;
static mem_t *s_mem;     /* a ring of the newest MEM_MAX exchanges */
static int s_mem_n, s_mem_head;

static void mem_push(const char *q, const char *a)
{
    if (!s_mem) s_mem = heap_caps_calloc(MEM_MAX, sizeof(mem_t), MALLOC_CAP_SPIRAM);
    if (!s_mem) return;
    int slot = (s_mem_head + s_mem_n) % MEM_MAX;
    if (s_mem_n == MEM_MAX) {   /* full: drop the oldest */
        free(s_mem[s_mem_head].q);
        free(s_mem[s_mem_head].a);
        s_mem_head = (s_mem_head + 1) % MEM_MAX;
        slot = (s_mem_head + s_mem_n - 1) % MEM_MAX;
    } else {
        s_mem_n++;
    }
    size_t nq = strlen(q), na = strlen(a);
    s_mem[slot].q = heap_caps_malloc(nq + 1, MALLOC_CAP_SPIRAM);
    s_mem[slot].a = heap_caps_malloc(na + 1, MALLOC_CAP_SPIRAM);
    if (s_mem[slot].q) memcpy(s_mem[slot].q, q, nq + 1);
    if (s_mem[slot].a) memcpy(s_mem[slot].a, a, na + 1);
}

static void mem_load_one(const char *q, const char *a, void *ctx)
{
    if (q) mem_push(q, a);
}

void history_recall_init(void)
{
    history_each(MEM_MAX, mem_load_one, NULL);
    ESP_LOGI(TAG, "%d exchanges to recall from", s_mem_n);
}

void history_recall_add(const char *q, const char *a) { mem_push(q, a); }

/* The question's two-character words (CJK only), less the ones every
 * question has. */
#define MAX_PAIRS 40
static int pairs_of(const char *text, char pairs[][7])
{
    static const char *COMMON[] = {"什么", "为什", "怎么", "一下", "我们", "你们", "是不", "不是", "可以",
                                   "这个", "那个", "知道", "现在", "今天", "有没", "没有", "一个", "的时",
                                   "时候", "你说", "我说", "告诉", "为啥", "是什"};
    int n = 0;
    const unsigned char *p = (const unsigned char *)text;
    while (*p && n < MAX_PAIRS) {
        /* a CJK ideograph is three bytes starting 0xE4..0xE9 */
        if (p[0] >= 0xE4 && p[0] <= 0xE9 && p[1] && p[2] && p[3] >= 0xE4 && p[3] <= 0xE9 && p[4] && p[5]) {
            memcpy(pairs[n], p, 6);
            pairs[n][6] = '\0';
            bool common = false;
            for (size_t k = 0; k < sizeof(COMMON) / sizeof(COMMON[0]); k++) {
                if (!strcmp(pairs[n], COMMON[k])) common = true;
            }
            if (!common) n++;
            p += 3;
        } else {
            p += *p < 0x80 ? 1 : *p < 0xE0 ? 2 : *p < 0xF0 ? 3 : 4;
        }
    }
    return n;
}

size_t history_recall(const char *question, int skip_newest, char *out, size_t cap)
{
    out[0] = '\0';
    if (!s_mem || !s_mem_n) return 0;
    char pairs[MAX_PAIRS][7];
    int np = pairs_of(question, pairs);
    if (np == 0) return 0;
    int best[3] = {-1, -1, -1}, best_s[3] = {0, 0, 0};
    for (int i = 0; i < s_mem_n - skip_newest; i++) {
        const mem_t *m = &s_mem[(s_mem_head + i) % MEM_MAX];
        if (!m->q || !m->a) continue;
        int score = 0;
        for (int k = 0; k < np; k++) {
            if (strstr(m->q, pairs[k])) score += 2;   /* a match in what was asked counts more */
            else if (strstr(m->a, pairs[k])) score += 1;
        }
        for (int r = 0; r < 3; r++) {
            if (score > best_s[r] || (score == best_s[r] && score > 0 && i > best[r])) {   /* ties: the newer */
                for (int t = 2; t > r; t--) {
                    best[t] = best[t - 1];
                    best_s[t] = best_s[t - 1];
                }
                best[r] = i;
                best_s[r] = score;
                break;
            }
        }
    }
    size_t n = 0;
    for (int r = 0; r < 3; r++) {
        if (best[r] < 0 || best_s[r] < 3) continue;   /* a real overlap, not one shared word */
        const mem_t *m = &s_mem[(s_mem_head + best[r]) % MEM_MAX];
        int w = snprintf(out + n, cap - n, "问：%.120s 答：%.300s\n", m->q, m->a);
        if (w < 0 || (size_t)w >= cap - n) break;
        n += w;
    }
    if (n) ESP_LOGI(TAG, "recalled %d bytes for this question", (int)n);
    return n;
}
