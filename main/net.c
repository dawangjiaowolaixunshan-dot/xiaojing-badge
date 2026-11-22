#include "net.h"

#include <stdio.h>
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
#include "freertos/semphr.h"
#include "settings.h"

static const char *TAG = "net";
static EventGroupHandle_t s_ev;
static SemaphoreHandle_t s_radio_mu;
static volatile bool s_suspended;   /* radio off while the screen is dark */

#define EV_UP     BIT0
#define EV_JOIN   BIT1   /* look for a known network and join it */
#define EV_FAIL   BIT2   /* STA disconnected unexpectedly during join */
#define EV_CANCEL BIT3   /* abort current join/backoff immediately */

/* Up to WIFI_SLOTS saved networks; the joiner scans and picks the strongest
 * one it knows, moving on to the next when one fails. */
static volatile int s_slot = -1;      /* the slot joined (or being joined) */
static volatile int s_prefer = -1;    /* try this slot first (just added / tapped) */
static volatile bool s_hold;          /* provisioning owns the radio */
static volatile bool s_scanning;      /* user-triggered STA scan in progress */
static volatile bool s_manual_disc;   /* ignore reason from our own esp_wifi_disconnect */
static volatile net_state_t s_state = NET_STATE_IDLE;
static volatile int s_last_reason;
static char s_target_ssid[33];

static bool any_saved_wifi(void)
{
    char s[33], p[65];
    for (int i = 0; i < WIFI_SLOTS; i++) {
        if (settings_get_wifi(i, s, sizeof(s), p, sizeof(p))) {
            memset(p, 0, sizeof(p));
            return true;
        }
    }
    memset(p, 0, sizeof(p));
    return false;
}

static bool apply_config(int slot)
{
    wifi_config_t wc = {0};
    char pass[65];
    if (!settings_get_wifi(slot, (char *)wc.sta.ssid, sizeof(wc.sta.ssid), pass, sizeof(pass))) {
        return false;
    }
    strlcpy(s_target_ssid, (char *)wc.sta.ssid, sizeof(s_target_ssid));
    strlcpy((char *)wc.sta.password, pass, sizeof(wc.sta.password));
    bool has_pass = pass[0] != '\0';
    memset(pass, 0, sizeof(pass));

    /* Phone hotspot compatibility (iPhone / Android WPA2 & WPA2/WPA3 transitional). */
    wc.sta.threshold.authmode = has_pass ? WIFI_AUTH_WEP : WIFI_AUTH_OPEN;
    wc.sta.pmf_cfg.capable = true;
    wc.sta.pmf_cfg.required = false;
#ifdef WIFI_SAE_PWE_BOTH
    wc.sta.sae_pwe_h2e = WIFI_SAE_PWE_BOTH;
#else
    wc.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
#endif
    wc.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wc.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;

    ESP_LOGI(TAG, "joining \"%s\" (slot %d, %s)", (char *)wc.sta.ssid, slot, has_pass ? "secured" : "open");
    esp_wifi_set_config(WIFI_IF_STA, &wc);
    memset(&wc, 0, sizeof(wc));
    s_slot = slot;
    return true;
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (!s_hold && !s_scanning) xEventGroupSetBits(s_ev, EV_JOIN);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        bool was_up = (xEventGroupGetBits(s_ev) & EV_UP) != 0;
        xEventGroupClearBits(s_ev, EV_UP);
        if (s_suspended || s_hold || s_scanning) return;
        wifi_event_sta_disconnected_t *d = data;
        if (s_manual_disc && d->reason == WIFI_REASON_ASSOC_LEAVE) {
            return;
        }
        s_last_reason = d->reason;
        ESP_LOGW(TAG, "disconnected (reason %d)", d->reason);
        xEventGroupSetBits(s_ev, EV_FAIL);
        if (was_up) {
            s_state = NET_STATE_CONNECTING;
            xEventGroupSetBits(s_ev, EV_JOIN);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        ESP_LOGI(TAG, "got ip " IPSTR, IP2STR(&e->ip_info.ip));
        s_last_reason = 0;
        s_state = NET_STATE_CONNECTED;
        xEventGroupSetBits(s_ev, EV_UP);
    }
}

/* The known networks in range, strongest first. Caller holds s_radio_mu. */
static int known_in_range(int *slots, int cap)
{
    uint16_t n = 0;
    wifi_scan_config_t sc = {
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active = {.min = 80, .max = 180},
    };
    if (esp_wifi_scan_start(&sc, true) != ESP_OK) return 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n > 32) n = 32;
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

static bool try_connect_slot(int slot)
{
    if (s_suspended || s_hold || s_scanning) return false;
    if (!apply_config(slot)) return false;

    s_state = NET_STATE_CONNECTING;
    s_manual_disc = false;
    s_last_reason = 0;
    xEventGroupClearBits(s_ev, EV_UP | EV_FAIL | EV_CANCEL);

    net_awake(true);
    esp_err_t rc = esp_wifi_connect();
    bool up = false;
    if (rc == ESP_OK) {
        EventBits_t b = xEventGroupWaitBits(s_ev, EV_UP | EV_FAIL | EV_CANCEL, pdFALSE, pdFALSE, pdMS_TO_TICKS(12000));
        up = (b & EV_UP) != 0;
        if (!up && !(b & EV_FAIL) && !(b & EV_CANCEL)) {
            s_last_reason = WIFI_REASON_BEACON_TIMEOUT;
        }
    } else {
        ESP_LOGW(TAG, "esp_wifi_connect failed: %s", esp_err_to_name(rc));
        s_last_reason = WIFI_REASON_ASSOC_FAIL;
    }
    if (!up) {
        s_manual_disc = true;
        esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(80));
        s_manual_disc = false;
    }
    net_awake(false);
    return up;
}

static void join_task(void *arg)
{
    for (;;) {
        xEventGroupWaitBits(s_ev, EV_JOIN, pdTRUE, pdFALSE, portMAX_DELAY);
        xEventGroupClearBits(s_ev, EV_CANCEL);
        if (s_suspended || s_hold || s_scanning) continue;
        if (net_up() && s_prefer < 0) {
            s_state = NET_STATE_CONNECTED;
            continue;
        }

        if (!any_saved_wifi()) {
            s_slot = -1;
            s_target_ssid[0] = '\0';
            s_last_reason = 0;
            s_state = NET_STATE_IDLE;
            continue;
        }

        xSemaphoreTake(s_radio_mu, portMAX_DELAY);
        if (s_suspended || s_hold || s_scanning) {
            xSemaphoreGive(s_radio_mu);
            continue;
        }

        /* Ensure we stay in pure STA receiver mode whenever provisioning isn't active. */
        wifi_mode_t mode;
        if (esp_wifi_get_mode(&mode) == ESP_OK && mode != WIFI_MODE_STA) {
            ESP_LOGW(TAG, "forcing pure STA mode (was %d)", mode);
            esp_wifi_set_mode(WIFI_MODE_STA);
        }

        int prefer = s_prefer;
        s_prefer = -1;
        bool up = false;
        bool tried_prefer = false;

        if (prefer >= 0) {
            /* Direct attempt first so newly selected / added hotspots connect quickly
             * without waiting for an extra scan, and never get dropped when other
             * networks are nearby. */
            if (net_up()) {
                s_manual_disc = true;
                esp_wifi_disconnect();
                xEventGroupClearBits(s_ev, EV_UP);
                vTaskDelay(pdMS_TO_TICKS(100));
                s_manual_disc = false;
            }
            tried_prefer = true;
            up = try_connect_slot(prefer);
        }

        int n = 0;
        if (!up && !s_suspended && !s_hold && !s_scanning && !(xEventGroupGetBits(s_ev) & EV_CANCEL)) {
            int slots[WIFI_SLOTS];
            s_state = NET_STATE_SCANNING;
            n = known_in_range(slots, WIFI_SLOTS);
            for (int i = 0; i < n && !up && !s_suspended && !s_hold && !s_scanning; i++) {
                if (tried_prefer && slots[i] == prefer) continue;
                if (xEventGroupGetBits(s_ev) & EV_CANCEL) break;
                up = try_connect_slot(slots[i]);
            }
            /* If user explicitly asked for a slot that wasn't up yet, keep its
             * failure reason; otherwise report no known AP found. */
            if (!up && n == 0 && !tried_prefer) {
                s_last_reason = WIFI_REASON_NO_AP_FOUND;
            }
        }
        xSemaphoreGive(s_radio_mu);

        if (!up && !s_suspended && !s_hold && !s_scanning) {
            s_state = NET_STATE_FAILED;
            ESP_LOGW(TAG, "%s (reason %d)",
                     (n || tried_prefer) ? "join failed; retrying soon" : "no known network in range",
                     s_last_reason);
            /* Interruptible backoff: net_join/net_reconnect wakes immediately. */
            int wait_ms = (n || tried_prefer) ? 5000 : 10000;
            EventBits_t b = xEventGroupWaitBits(s_ev, EV_JOIN | EV_CANCEL, pdFALSE, pdFALSE, pdMS_TO_TICKS(wait_ms));
            if (!(b & (EV_JOIN | EV_CANCEL)) && !s_suspended && !s_hold && !s_scanning) {
                xEventGroupSetBits(s_ev, EV_JOIN);
            }
        }
    }
}

void net_start(void)
{
    s_ev = xEventGroupCreate();
    s_radio_mu = xSemaphoreCreateMutex();
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
    esp_wifi_set_max_tx_power(80);       /* 20 dBm max TX power for weak-signal stability */
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
    s_last_reason = 0;
    s_manual_disc = true;
    esp_wifi_disconnect();
    xEventGroupClearBits(s_ev, EV_UP);
    xEventGroupSetBits(s_ev, EV_CANCEL | EV_JOIN);
}

void net_join(int slot)
{
    s_prefer = slot;
    s_last_reason = 0;
    if (s_suspended) net_suspend(false);
    s_manual_disc = true;
    esp_wifi_disconnect();
    xEventGroupClearBits(s_ev, EV_UP);
    s_state = NET_STATE_CONNECTING;
    xEventGroupSetBits(s_ev, EV_CANCEL | EV_JOIN);
}

void net_hold(bool hold)
{
    s_hold = hold;
    if (hold) {
        xEventGroupSetBits(s_ev, EV_CANCEL);
    } else {
        wifi_mode_t mode;
        if (esp_wifi_get_mode(&mode) == ESP_OK && mode != WIFI_MODE_STA) {
            esp_wifi_set_mode(WIFI_MODE_STA);
        }
        xEventGroupSetBits(s_ev, EV_JOIN);
    }
}

int net_slot(void)
{
    return s_slot;
}

bool net_wait(int timeout_ms)
{
    TickType_t ticks = (timeout_ms < 0 || (uint32_t)timeout_ms == portMAX_DELAY)
                           ? portMAX_DELAY
                           : pdMS_TO_TICKS(timeout_ms);
    return (xEventGroupWaitBits(s_ev, EV_UP, pdFALSE, pdTRUE, ticks) & EV_UP) != 0;
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
 * (speech recognition, reading aloud) or while joining an AP. Counted, so
 * overlapping users nest. */
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
        xEventGroupSetBits(s_ev, EV_CANCEL);
        esp_wifi_stop();
        ESP_LOGI(TAG, "radio off (screen dark)");
    } else {
        esp_wifi_start();   /* STA_START connects */
        esp_wifi_set_max_tx_power(80);
        ESP_LOGI(TAG, "radio on");
    }
}

int net_scan(net_ap_t *out, int cap)
{
    if (!out || cap <= 0 || !s_radio_mu) return 0;
    s_scanning = true;
    xEventGroupSetBits(s_ev, EV_CANCEL);
    if (s_suspended) net_suspend(false);

    xSemaphoreTake(s_radio_mu, portMAX_DELAY);
    if (!s_hold) {
        wifi_mode_t mode;
        if (esp_wifi_get_mode(&mode) == ESP_OK && mode != WIFI_MODE_STA) {
            esp_wifi_set_mode(WIFI_MODE_STA);
        }
    }
    if (!net_up()) {
        s_manual_disc = true;
        esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(100));
        s_manual_disc = false;
    }

    wifi_scan_config_t sc = {
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active = {.min = 60, .max = 120},
    };
    esp_err_t err = esp_wifi_scan_start(&sc, true);
    if (err == ESP_ERR_WIFI_STATE) {
        vTaskDelay(pdMS_TO_TICKS(150));
        err = esp_wifi_scan_start(&sc, true);
    }

    int k = 0;
    if (err == ESP_OK) {
        uint16_t n = 0;
        esp_wifi_scan_get_ap_num(&n);
        if (n > 32) n = 32;
        wifi_ap_record_t *aps = calloc(n ? n : 1, sizeof(wifi_ap_record_t));
        if (aps) {
            esp_wifi_scan_get_ap_records(&n, aps);
            for (int i = 0; i < n && k < cap; i++) {
                const char *ss = (const char *)aps[i].ssid;
                if (!ss[0]) continue;
                bool dup = false;
                for (int j = 0; j < k; j++) {
                    if (!strcmp(out[j].ssid, ss)) {
                        dup = true;
                        break;
                    }
                }
                if (dup) continue;
                strlcpy(out[k].ssid, ss, sizeof(out[k].ssid));
                out[k].rssi = aps[i].rssi;
                out[k].secured = aps[i].authmode != WIFI_AUTH_OPEN;
                out[k].saved_slot = (int8_t)settings_find_wifi(ss);
                k++;
            }
            free(aps);
        }
    } else {
        ESP_LOGW(TAG, "scan failed: %s", esp_err_to_name(err));
    }

    xSemaphoreGive(s_radio_mu);
    s_scanning = false;
    if (!net_up() && !s_suspended && !s_hold && any_saved_wifi()) {
        xEventGroupSetBits(s_ev, EV_JOIN);
    }
    ESP_LOGI(TAG, "scan found %d networks", k);
    return k;
}

net_state_t net_state(void)
{
    if (net_up()) return NET_STATE_CONNECTED;
    return s_state;
}

int net_last_reason(void)
{
    return s_last_reason;
}

void net_target_ssid(char *out, size_t cap)
{
    if (!out || cap == 0) return;
    if (s_target_ssid[0]) {
        strlcpy(out, s_target_ssid, cap);
    } else if (s_slot >= 0) {
        char pass[65];
        if (!settings_get_wifi(s_slot, out, cap, pass, sizeof(pass))) out[0] = '\0';
        memset(pass, 0, sizeof(pass));
    } else {
        out[0] = '\0';
    }
}

const char *net_status_text(char *buf, size_t cap)
{
    if (!buf || cap == 0) return "";
    char ssid[33] = "";
    net_target_ssid(ssid, sizeof(ssid));
    if (net_up()) {
        snprintf(buf, cap, "已连上：%s\n信号 %d dBm", ssid[0] ? ssid : "Wi-Fi", net_rssi());
        return buf;
    }
    if (!any_saved_wifi()) {
        snprintf(buf, cap, "未添加 Wi-Fi");
        return buf;
    }
    if (s_state == NET_STATE_SCANNING) {
        snprintf(buf, cap, "正在寻找已存网络…");
        return buf;
    }
    if (s_state == NET_STATE_CONNECTING) {
        snprintf(buf, cap, "正在连接：%s…", ssid[0] ? ssid : "Wi-Fi");
        return buf;
    }
    int r = s_last_reason;
    const char *why = "未连上网络";
    switch (r) {
    case WIFI_REASON_NO_AP_FOUND:
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
        why = "找不到热点（请开2.4G/最大兼容性）";
        break;
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_MIC_FAILURE:
    case WIFI_REASON_AUTH_EXPIRE:
    case WIFI_REASON_GROUP_KEY_UPDATE_TIMEOUT:
        why = "密码不对或认证失败";
        break;
    case WIFI_REASON_ASSOC_FAIL:
    case WIFI_REASON_CONNECTION_FAIL:
    case WIFI_REASON_BEACON_TIMEOUT:
        why = "信号弱或连接超时";
        break;
    default:
        break;
    }
    if (ssid[0]) snprintf(buf, cap, "未连上：%s\n%s", ssid, why);
    else snprintf(buf, cap, "%s", why);
    return buf;
}
