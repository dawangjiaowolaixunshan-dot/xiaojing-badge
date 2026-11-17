#include "net.h"

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
#define EV_UP BIT0

/* Slot 0 is home Wi-Fi, slot 1 the phone hotspot. After FAILS_BEFORE_SWITCH
 * failed tries on one, try the other (if it is set). */
#define FAILS_BEFORE_SWITCH 3
static volatile int s_slot;
static volatile int s_fails;

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
    ESP_LOGI(TAG, "joining \"%s\" (%s)", (char *)wc.sta.ssid, slot ? "hotspot" : "home");
    esp_wifi_set_config(WIFI_IF_STA, &wc);
    memset(&wc, 0, sizeof(wc));
    s_slot = slot;
    s_fails = 0;
    return true;
}

/* Home first; the hotspot only when home is not set. */
static void apply_preferred(void)
{
    if (!apply_config(0) && !apply_config(1)) {
        ESP_LOGW(TAG, "no Wi-Fi saved: set it with tools/badge_setup.py");
    }
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_ev, EV_UP);
        wifi_event_sta_disconnected_t *d = data;
        ESP_LOGW(TAG, "disconnected (reason %d); retrying", d->reason);
        /* A disconnect we asked for (net_reconnect) is not a failure. */
        if (d->reason != WIFI_REASON_ASSOC_LEAVE && ++s_fails >= FAILS_BEFORE_SWITCH) {
            int other = !s_slot;
            if (apply_config(other)) {
                ESP_LOGW(TAG, "switching to %s", other ? "hotspot" : "home Wi-Fi");
            } else {
                s_fails = 0;   /* nothing to switch to: keep retrying this one */
            }
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        ESP_LOGI(TAG, "got ip " IPSTR, IP2STR(&e->ip_info.ip));
        s_fails = 0;
        xEventGroupSetBits(s_ev, EV_UP);
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
    apply_preferred();
    ESP_ERROR_CHECK(esp_wifi_start());
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
    esp_wifi_disconnect();
    apply_preferred();
    esp_wifi_connect();
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
