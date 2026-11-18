#include "net.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "settings.h"

static const char *TAG = "net";
static EventGroupHandle_t s_ev;
static volatile bool s_suspended;   /* radio off while the screen is dark */
#define EV_UP BIT0
#define EV_JOIN BIT1   /* look for a known network and join it */

/* Up to WIFI_SLOTS saved networks; the joiner scans and picks the strongest
 * one it knows, moving on to the next when one fails. */
static volatile int s_slot = -1;      /* the slot joined (or being joined) */
static volatile int s_prefer = -1;    /* try this slot first (just added) */
static volatile bool s_hold;          /* provisioning owns the radio */

static bool apply_config(int slot)
{
    wifi_config_t wc = {0};
    char pass[65];
    if (!settings_get_wifi(slot, (char *)wc.sta.ssid, sizeof(wc.sta.ssid), pass, sizeof(pass))) {
        return false;
    }
    strlcpy((char *)wc.sta.password, pass, sizeof(wc.sta.password));
    memset(pass, 0, sizeof(pass));
    wc.sta.threshold.authmode = WIFI_AUTH_OPEN;
    wc.sta.pmf_cfg.capable = true;
    ESP_LOGI(TAG, "joining \"%s\" (slot %d)", (char *)wc.sta.ssid, slot);
    esp_wifi_set_config(WIFI_IF_STA, &wc);
    memset(&wc, 0, sizeof(wc));
    s_slot = slot;
    return true;
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (!s_hold) xEventGroupSetBits(s_ev, EV_JOIN);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_ev, EV_UP);
        if (s_suspended || s_hold) return;
        wifi_event_sta_disconnected_t *d = data;
        ESP_LOGW(TAG, "disconnected (reason %d)", d->reason);
        xEventGroupSetBits(s_ev, EV_JOIN);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        ESP_LOGI(TAG, "got ip " IPSTR, IP2STR(&e->ip_info.ip));
        xEventGroupSetBits(s_ev, EV_UP);
    }
}

/* The known networks in range, strongest first. */
static int known_in_range(int *slots, int cap)
{
    uint16_t n = 0;
    if (esp_wifi_scan_start(NULL, true) != ESP_OK) return 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n > 24) n = 24;
    wifi_ap_record_t *aps = calloc(n ? n : 1, sizeof(wifi_ap_record_t));
    if (!aps) return 0;
    esp_wifi_scan_get_ap_records(&n, aps);   /* sorted by signal, strongest first */
    int k = 0;
    for (int i = 0; i < n && k < cap; i++) {
        int slot = settings_find_wifi((const char *)aps[i].ssid);
        bool dup = false;
        for (int j = 0; j < k; j++) dup |= slots[j] == slot;
        if (slot >= 0 && !dup) slots[k++] = slot;
    }
    free(aps);
    return k;
}

static void join_task(void *arg)
{
    for (;;) {
        xEventGroupWaitBits(s_ev, EV_JOIN, pdTRUE, pdFALSE, portMAX_DELAY);
        if (s_suspended || s_hold) continue;
        vTaskDelay(pdMS_TO_TICKS(500));
        int slots[WIFI_SLOTS], n = known_in_range(slots, WIFI_SLOTS);
        if (s_prefer >= 0) {   /* the one just added goes first */
            for (int i = n - 1; i > 0; i--) if (slots[i] == s_prefer) { slots[i] = slots[0]; slots[0] = s_prefer; }
            if (n == 0) slots[n++] = s_prefer;
            s_prefer = -1;
        }
        bool up = false;
        for (int i = 0; i < n && !up && !s_suspended && !s_hold; i++) {
            if (!apply_config(slots[i])) continue;
            esp_wifi_connect();
            up = xEventGroupWaitBits(s_ev, EV_UP, pdFALSE, pdFALSE, pdMS_TO_TICKS(12000)) & EV_UP;
            if (!up) esp_wifi_disconnect();
        }
        if (!up && !s_suspended && !s_hold) {
            ESP_LOGW(TAG, "%s", n ? "no known network would take us; trying again soon" : "no known network in range");
            vTaskDelay(pdMS_TO_TICKS(n ? 5000 : 15000));
            xEventGroupSetBits(s_ev, EV_JOIN);
        }
    }
}

void net_start(void)
{
    s_ev = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    xTaskCreate(join_task, "netjoin", 4096, NULL, 4, NULL);
    ESP_ERROR_CHECK(esp_wifi_start());   /* STA_START sets the joiner going */
    /* The radio sleeps between beacons: the biggest saving on a 500 mAh
     * battery, for tens of ms more latency. */
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);

    setenv("TZ", "CST-8", 1);
    tzset();
    esp_sntp_config_t sc = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(2,
        ESP_SNTP_SERVER_LIST("ntp.aliyun.com", "ntp.tencent.com"));
    esp_netif_sntp_init(&sc);
}

void net_reconnect(void)
{
    esp_wifi_disconnect();   /* DISCONNECTED sets the joiner going */
    xEventGroupSetBits(s_ev, EV_JOIN);
}

void net_join(int slot)
{
    s_prefer = slot;
    net_reconnect();
}

void net_hold(bool hold)
{
    s_hold = hold;
    if (!hold) xEventGroupSetBits(s_ev, EV_JOIN);
}

int net_slot(void)
{
    return s_slot;
}

bool net_wait(int timeout_ms)
{
    return xEventGroupWaitBits(s_ev, EV_UP, pdFALSE, pdTRUE, pdMS_TO_TICKS(timeout_ms)) & EV_UP;
}

bool net_up(void)
{
    return s_ev && (xEventGroupGetBits(s_ev) & EV_UP);
}

bool net_time_valid(void)
{
    return time(NULL) > 1767225600;   /* 2026-01-01 */
}

int net_rssi(void)
{
    wifi_ap_record_t ap;
    return net_up() && esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : -127;
}

/* Modem sleep saves battery but delays packets: off while audio streams
 * (speech recognition, reading aloud). Counted, so overlapping users nest. */
void net_awake(bool on)
{
    static int users;
    static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
    taskENTER_CRITICAL(&mux);
    users += on ? 1 : -1;
    if (users < 0) users = 0;
    int now = users;
    taskEXIT_CRITICAL(&mux);
    esp_wifi_set_ps(now ? WIFI_PS_NONE : WIFI_PS_MIN_MODEM);
}

void net_suspend(bool off)
{
    if (off == s_suspended) return;
    s_suspended = off;
    if (off) {
        esp_wifi_stop();
        ESP_LOGI(TAG, "radio off (screen dark)");
    } else {
        esp_wifi_start();   /* STA_START connects */
        ESP_LOGI(TAG, "radio on");
    }
}
