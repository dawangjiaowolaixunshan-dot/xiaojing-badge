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

void history_add(const char *q, const char *a)
{
    if (!s_ok) return;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "q", q);
    cJSON_AddStringToObject(o, "a", a);
    time_t now = time(NULL);
    if (now > 1767225600) cJSON_AddNumberToObject(o, "t", (double)now);   /* once the clock is set */
    char *line = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    xSemaphoreTake(s_mu, portMAX_DELAY);
    if (line) append_line(line);
    trim();
    xSemaphoreGive(s_mu);
    cJSON_free(line);
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
