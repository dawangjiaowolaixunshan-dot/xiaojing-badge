#include "wifi_prov.h"

#include <stdio.h>
#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "net.h"
#include "settings.h"

static const char *TAG = "prov";
#define MAX_SCAN 20
#define TIMEOUT_US (5LL * 60 * 1000000)

static esp_netif_t *s_ap_netif;
static httpd_handle_t s_http;
static volatile bool s_on, s_dns_run;
static char s_ssid[24], s_pass[12];
static char s_seen[MAX_SCAN][33];
static int s_nseen;
static prov_status_cb_t s_cb;
static esp_timer_handle_t s_timer;
static int s_saved_slot = -1;

const char *prov_ap_ssid(void) { return s_ssid; }
const char *prov_ap_pass(void) { return s_pass; }

/* ---- DNS: every name is the badge, so the phone shows the page ---- */

static void dns_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(53), .sin_addr.s_addr = htonl(INADDR_ANY)};
    struct timeval tv = {.tv_sec = 1};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    if (sock < 0 || bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "dns: can't bind");
        if (sock >= 0) close(sock);
        vTaskDelete(NULL);
    }
    uint8_t buf[512];
    while (s_dns_run) {
        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        int n = recvfrom(sock, buf, sizeof(buf) - 16, 0, (struct sockaddr *)&from, &fl);
        if (n < 12) continue;
        /* the question ends after its name's zero byte, then type and class */
        int q = 12;
        while (q < n && buf[q]) q += buf[q] + 1;
        if (q + 5 > n) continue;
        uint16_t qtype = buf[q + 1] << 8 | buf[q + 2];
        int end = q + 5;
        buf[2] = 0x81;   /* a response, recursion available, no error */
        buf[3] = 0x80;
        buf[6] = 0;      /* answers */
        buf[7] = qtype == 1 ? 1 : 0;
        buf[8] = buf[9] = buf[10] = buf[11] = 0;
        int len = end;
        if (qtype == 1) {   /* A: 192.168.4.1 */
            static const uint8_t ans[] = {0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4, 192, 168, 4, 1};
            memcpy(buf + end, ans, sizeof(ans));
            len += sizeof(ans);
        }
        sendto(sock, buf, len, 0, (struct sockaddr *)&from, fl);
    }
    close(sock);
    vTaskDelete(NULL);
}

/* ---- the page ---- */

static void html_escape(char *out, size_t cap, const char *in)
{
    size_t o = 0;
    for (; *in && o + 7 < cap; in++) {
        const char *r = *in == '<' ? "&lt;" : *in == '>' ? "&gt;" : *in == '&' ? "&amp;" : *in == '"' ? "&quot;"
                      : *in == '\'' ? "&#39;" : NULL;
        if (r) {
            strcpy(out + o, r);
            o += strlen(r);
        } else {
            out[o++] = *in;
        }
    }
    out[o] = '\0';
}

static const char *HEAD =
    "<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>小鲸 · 添加 Wi-Fi</title><style>body{font-family:-apple-system,sans-serif;background:#0b0f19;color:#eef;"
    "margin:0;padding:20px}h1{font-size:22px;color:#e08a45}label{display:block;padding:12px;margin:8px 0;"
    "background:#151c2c;border-radius:12px}input[type=text],input[type=password]{width:100%;box-sizing:border-box;"
    "padding:12px;margin:6px 0 14px;border-radius:10px;border:1px solid #334;background:#0f1422;color:#fff;font-size:17px}"
    "button{width:100%;padding:14px;border:0;border-radius:12px;background:#2f5bea;color:#fff;font-size:18px}"
    "small{color:#8a93a8}</style></head><body>";

static esp_err_t page(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr_chunk(req, HEAD);
    httpd_resp_sendstr_chunk(req, "<h1>给小鲸连 Wi-Fi</h1><form method='post' action='/save'><small>选一个网络：</small>");
    char row[512], esc[200];
    for (int i = 0; i < s_nseen; i++) {
        html_escape(esc, sizeof(esc), s_seen[i]);
        snprintf(row, sizeof(row), "<label><input type='radio' name='ssid' value=\"%s\"%s> %s</label>", esc,
                 i == 0 ? " checked" : "", esc);
        httpd_resp_sendstr_chunk(req, row);
    }
    httpd_resp_sendstr_chunk(req,
        "<small>不在上面？手动填名字：</small><input type='text' name='other' placeholder='Wi-Fi 名称（可不填）'>"
        "<small>密码：</small><input type='password' name='pass' placeholder='没有密码就空着'>"
        "<button type='submit'>保存并连接</button></form></body></html>");
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

static void url_decode(char *s)
{
    char *o = s;
    for (; *s; s++) {
        if (*s == '+') *o++ = ' ';
        else if (*s == '%' && s[1] && s[2]) {
            char h[3] = {s[1], s[2], 0};
            *o++ = (char)strtol(h, NULL, 16);
            s += 2;
        } else *o++ = *s;
    }
    *o = '\0';
}

/* key=value from a form body, decoded; false if absent. */
static bool form_get(const char *body, const char *key, char *out, size_t cap)
{
    size_t kl = strlen(key);
    for (const char *p = body; p && *p; p = strchr(p, '&') ? strchr(p, '&') + 1 : NULL) {
        if (!strncmp(p, key, kl) && p[kl] == '=') {
            const char *v = p + kl + 1, *e = strchr(v, '&');
            size_t n = e ? (size_t)(e - v) : strlen(v);
            if (n >= cap) n = cap - 1;
            memcpy(out, v, n);
            out[n] = '\0';
            url_decode(out);
            return true;
        }
    }
    out[0] = '\0';
    return false;
}

static void stop_later(void *arg)
{
    int slot = s_saved_slot;
    prov_stop();
    if (slot >= 0) net_join(slot);
}

static esp_err_t save(httpd_req_t *req)
{
    char body[512] = "", ssid[33], other[33], pass[65];
    int n = httpd_req_recv(req, body, sizeof(body) - 1);
    if (n <= 0) return ESP_FAIL;
    body[n] = '\0';
    form_get(body, "ssid", ssid, sizeof(ssid));
    form_get(body, "other", other, sizeof(other));
    form_get(body, "pass", pass, sizeof(pass));
    memset(body, 0, sizeof(body));
    const char *use = other[0] ? other : ssid;
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr_chunk(req, HEAD);
    if (!use[0]) {
        httpd_resp_sendstr_chunk(req, "<h1>没选网络</h1><p><a href='/' style='color:#8cf'>返回</a></p></body></html>");
        httpd_resp_sendstr_chunk(req, NULL);
        return ESP_OK;
    }
    s_saved_slot = settings_add_wifi(use, pass);
    memset(pass, 0, sizeof(pass));
    char esc[100], msg[256];
    html_escape(esc, sizeof(esc), use);
    snprintf(msg, sizeof(msg), "<h1>已保存</h1><p>小鲸正在连接 <b>%s</b>，看吧唧屏幕上的提示。这个页面可以关掉了。</p></body></html>", esc);
    httpd_resp_sendstr_chunk(req, msg);
    httpd_resp_sendstr_chunk(req, NULL);
    ESP_LOGI(TAG, "saved \"%s\" in slot %d", use, s_saved_slot);
    if (s_cb) {
        snprintf(msg, sizeof(msg), "已保存「%s」，正在连接…", use);
        s_cb(msg, false);
    }
    /* Leave the AP up long enough for the reply to reach the phone. */
    esp_timer_handle_t t;
    esp_timer_create_args_t a = {.callback = stop_later, .name = "prov_stop"};
    if (esp_timer_create(&a, &t) == ESP_OK) esp_timer_start_once(t, 2500 * 1000);
    return ESP_OK;
}

/* ---- start / stop ---- */

static void scan_nearby(void)
{
    uint16_t n = 0;
    s_nseen = 0;
    if (esp_wifi_scan_start(NULL, true) != ESP_OK) return;
    esp_wifi_scan_get_ap_num(&n);
    if (n > 30) n = 30;
    wifi_ap_record_t *aps = calloc(n ? n : 1, sizeof(wifi_ap_record_t));
    if (!aps) return;
    esp_wifi_scan_get_ap_records(&n, aps);
    for (int i = 0; i < n && s_nseen < MAX_SCAN; i++) {
        const char *ss = (const char *)aps[i].ssid;
        bool dup = !ss[0];
        for (int j = 0; j < s_nseen && !dup; j++) dup = !strcmp(s_seen[j], ss);
        if (!dup) strlcpy(s_seen[s_nseen++], ss, 33);
    }
    free(aps);
}

static void on_timeout(void *arg)
{
    if (s_cb) s_cb("5 分钟没人设置，已关闭", true);
    prov_stop();
}

bool prov_start(prov_status_cb_t cb)
{
    if (s_on) return true;
    s_cb = cb;
    s_saved_slot = -1;
    net_hold(true);
    net_suspend(false);
    esp_wifi_disconnect();
    if (!s_ap_netif) s_ap_netif = esp_netif_create_default_wifi_ap();
    esp_wifi_set_mode(WIFI_MODE_APSTA);
    scan_nearby();

    snprintf(s_ssid, sizeof(s_ssid), "XiaoJing-%04u", (unsigned)(esp_random() % 10000));
    snprintf(s_pass, sizeof(s_pass), "%08u", (unsigned)(esp_random() % 100000000));
    wifi_config_t ap = {.ap = {.max_connection = 2, .authmode = WIFI_AUTH_WPA2_PSK}};
    strlcpy((char *)ap.ap.ssid, s_ssid, sizeof(ap.ap.ssid));
    ap.ap.ssid_len = strlen(s_ssid);
    strlcpy((char *)ap.ap.password, s_pass, sizeof(ap.ap.password));
    if (esp_wifi_set_config(WIFI_IF_AP, &ap) != ESP_OK) {
        ESP_LOGE(TAG, "can't start the access point");
        prov_stop();
        return false;
    }

    httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
    hc.uri_match_fn = httpd_uri_match_wildcard;
    hc.lru_purge_enable = true;
    hc.stack_size = 6144;
    if (httpd_start(&s_http, &hc) == ESP_OK) {
        httpd_uri_t save_uri = {.uri = "/save", .method = HTTP_POST, .handler = save};
        httpd_uri_t any = {.uri = "/*", .method = HTTP_GET, .handler = page};   /* captive checks land here too */
        httpd_register_uri_handler(s_http, &save_uri);
        httpd_register_uri_handler(s_http, &any);
    }
    s_dns_run = true;
    xTaskCreate(dns_task, "dns", 3072, NULL, 4, NULL);
    esp_timer_create_args_t ta = {.callback = on_timeout, .name = "prov_timeout"};
    if (!s_timer) esp_timer_create(&ta, &s_timer);
    esp_timer_start_once(s_timer, TIMEOUT_US);
    s_on = true;
    ESP_LOGI(TAG, "access point %s up, %d networks seen", s_ssid, s_nseen);
    return true;
}

void prov_stop(void)
{
    if (s_timer) esp_timer_stop(s_timer);
    s_dns_run = false;
    if (s_http) {
        httpd_stop(s_http);
        s_http = NULL;
    }
    if (s_on) esp_wifi_set_mode(WIFI_MODE_STA);
    s_on = false;
    memset(s_pass, 0, sizeof(s_pass));
    net_hold(false);
}
