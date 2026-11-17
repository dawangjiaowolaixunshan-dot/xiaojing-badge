#include "weather.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "net.h"
#include "settings.h"

static const char *TAG = "weather";
#define LOC_URL "http://ip-api.com/json/?lang=zh-CN&fields=status,city,regionName,lat,lon"
#define BODY_CAP 8192

typedef struct {
    float lat, lon;
    char city[48];
    uint32_t when;   /* time() it was looked up */
} loc_t;

static weather_t s_w;
static SemaphoreHandle_t s_mu;

const char *weather_desc(int c)
{
    if (c == 0) return "晴";
    if (c == 1) return "晴间多云";
    if (c == 2) return "多云";
    if (c == 3) return "阴";
    if (c == 45 || c == 48) return "雾";
    if (c >= 51 && c <= 57) return "毛毛雨";
    if (c == 61 || c == 80) return "小雨";
    if (c == 63 || c == 81) return "中雨";
    if (c == 65 || c == 82) return "大雨";
    if (c == 66 || c == 67) return "冻雨";
    if (c == 71 || c == 85) return "小雪";
    if (c == 73) return "中雪";
    if (c == 75 || c == 86 || c == 77) return "大雪";
    if (c >= 95) return "雷雨";
    return "未知";
}

/* GET a URL into a PSRAM buffer; returns the length or -1. */
static int http_get(const char *url, char *body, int cap)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 10000,
        .buffer_size = 2048,
    };
    esp_http_client_handle_t h = esp_http_client_init(&cfg);
    int n = -1;
    if (esp_http_client_open(h, 0) == ESP_OK) {
        esp_http_client_fetch_headers(h);
        if (esp_http_client_get_status_code(h) == 200) {
            n = 0;
            int r;
            while (n < cap - 1 && (r = esp_http_client_read(h, body + n, cap - 1 - n)) > 0) n += r;
            body[n] = '\0';
        }
    }
    esp_http_client_cleanup(h);
    return n;
}

static bool locate(loc_t *loc, char *body)
{
    if (http_get(LOC_URL, body, BODY_CAP) <= 0) return false;
    cJSON *j = cJSON_Parse(body);
    bool ok = j && !strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(j, "status")) ?: "", "success");
    if (ok) {
        loc->lat = cJSON_GetNumberValue(cJSON_GetObjectItem(j, "lat"));
        loc->lon = cJSON_GetNumberValue(cJSON_GetObjectItem(j, "lon"));
        const char *city = cJSON_GetStringValue(cJSON_GetObjectItem(j, "city"));
        const char *region = cJSON_GetStringValue(cJSON_GetObjectItem(j, "regionName"));
        snprintf(loc->city, sizeof(loc->city), "%s", city && *city ? city : region ? region : "");
        loc->when = (uint32_t)time(NULL);
        ESP_LOGI(TAG, "located: %s", loc->city);
    }
    cJSON_Delete(j);
    return ok;
}

static bool fetch(const loc_t *loc, char *body)
{
    char url[320];
    snprintf(url, sizeof(url),
             "https://api.open-meteo.com/v1/forecast?latitude=%.3f&longitude=%.3f"
             "&current=temperature_2m,relative_humidity_2m,weather_code"
             "&daily=temperature_2m_max,temperature_2m_min,precipitation_probability_max"
             "&timezone=Asia%%2FShanghai&forecast_days=1",
             loc->lat, loc->lon);
    if (http_get(url, body, BODY_CAP) <= 0) return false;
    cJSON *j = cJSON_Parse(body);
    cJSON *cur = cJSON_GetObjectItem(j, "current"), *day = cJSON_GetObjectItem(j, "daily");
    bool ok = cur && day;
    if (ok) {
        weather_t w = {.valid = true};
        snprintf(w.city, sizeof(w.city), "%s", loc->city);
        w.temp = cJSON_GetNumberValue(cJSON_GetObjectItem(cur, "temperature_2m"));
        w.humidity = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(cur, "relative_humidity_2m"));
        w.code = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(cur, "weather_code"));
        w.tmax = cJSON_GetNumberValue(cJSON_GetArrayItem(cJSON_GetObjectItem(day, "temperature_2m_max"), 0));
        w.tmin = cJSON_GetNumberValue(cJSON_GetArrayItem(cJSON_GetObjectItem(day, "temperature_2m_min"), 0));
        double rain = cJSON_GetNumberValue(cJSON_GetArrayItem(cJSON_GetObjectItem(day, "precipitation_probability_max"), 0));
        w.rain_chance = isnan(rain) ? -1 : (int)rain;
        xSemaphoreTake(s_mu, portMAX_DELAY);
        s_w = w;
        xSemaphoreGive(s_mu);
        ESP_LOGI(TAG, "%s %.0f°C code %d", w.city, w.temp, w.code);
    }
    cJSON_Delete(j);
    return ok;
}

static void weather_task(void *arg)
{
    char *body = heap_caps_malloc(BODY_CAP, MALLOC_CAP_SPIRAM);
    loc_t loc = {0};
    bool have_loc = settings_get_blob("loc", &loc, sizeof(loc));
    for (;;) {
        net_wait(portMAX_DELAY);
        if (net_time_valid() && (!have_loc || time(NULL) - loc.when > 86400)) {
            if (locate(&loc, body)) {
                have_loc = true;
                settings_set_blob("loc", &loc, sizeof(loc));
            }
        }
        bool ok = have_loc && fetch(&loc, body);
        vTaskDelay(pdMS_TO_TICKS(ok ? 30 * 60 * 1000 : 60 * 1000));   /* retry sooner after a failure */
    }
}

void weather_start(void)
{
    s_mu = xSemaphoreCreateMutex();
    xTaskCreate(weather_task, "weather", 6144, NULL, 2, NULL);
}

bool weather_get(weather_t *out)
{
    if (!s_mu) return false;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    *out = s_w;
    xSemaphoreGive(s_mu);
    return out->valid;
}

size_t weather_line(char *buf, size_t cap)
{
    weather_t w;
    buf[0] = '\0';
    if (!weather_get(&w)) return 0;
    int n = snprintf(buf, cap, "%s：%s，%.0f°C，今天 %.0f～%.0f°C", w.city, weather_desc(w.code), w.temp, w.tmin, w.tmax);
    if (w.rain_chance >= 0 && n < (int)cap) n += snprintf(buf + n, cap - n, "，降水概率 %d%%", w.rain_chance);
    return n;
}
