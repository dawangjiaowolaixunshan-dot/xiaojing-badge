#include "console.h"

#include <stdio.h>
#include <string.h>

#include "driver/usb_serial_jtag.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "history.h"
#include "net.h"
#include "settings.h"
#include "ui.h"

static const char *TAG = "console";
#define CMD_LINE_MAX 512

static void wipe(char *s, size_t n)
{
    volatile char *p = s;
    while (n--) *p++ = 0;
}

/* The prompt arrives in base64 pieces, one line each. */
static char *s_prompt;
static size_t s_prompt_len;
static bool s_prompt_bad;

static int b64_val(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static int b64_decode(const char *in, char *out, size_t cap)
{
    size_t o = 0;
    uint32_t acc = 0;
    int bits = 0;
    for (; *in && *in != '='; in++) {
        int v = b64_val(*in);
        if (v < 0) return -1;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (o >= cap) return -1;
            out[o++] = (char)((acc >> bits) & 0xff);
        }
    }
    return (int)o;
}

static int s_dump_n;

static void dump_line(const char *line)
{
    static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t n = strlen(line);
    fputs("@h ", stdout);
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint8_t)line[i] << 16;
        if (i + 1 < n) v |= (uint8_t)line[i + 1] << 8;
        if (i + 2 < n) v |= (uint8_t)line[i + 2];
        char out[5] = {T[v >> 18 & 63], T[v >> 12 & 63], i + 1 < n ? T[v >> 6 & 63] : '=', i + 2 < n ? T[v & 63] : '=', 0};
        fputs(out, stdout);
    }
    fputc('\n', stdout);
    fflush(stdout);
    s_dump_n++;
    vTaskDelay(1);   /* let the USB FIFO drain */
}

static void handle(char *line)
{
    if (line[0] != '>') {
        return;
    }
    line++;
    if (!strncmp(line, "ds_key=", 7) || !strncmp(line, "bl_key=", 7)) {
        key_id_t id = line[0] == 'd' ? KEY_DEEPSEEK : KEY_BAILIAN;
        bool ok = settings_set_key(id, line + 7);
        printf("@%s %.6s\n", ok ? "ok" : "fail", line);
        ui_settings_changed();
    } else if (!strncmp(line, "wifi=", 5)) {
        char *ssid = line + 5, *tab = strchr(ssid, '\t');
        if (!tab) {
            printf("@fail wifi (ssid<TAB>password)\n");
            return;
        }
        *tab = '\0';
        bool ok = settings_set_wifi(ssid, tab + 1);
        printf("@%s wifi\n", ok ? "ok" : "fail");
        if (ok) {
            net_reconnect();
        }
    } else if (!strcmp(line, "keys?")) {
        char *v = heap_caps_malloc(CMD_LINE_MAX, MALLOC_CAP_SPIRAM);
        char pass[65];
        if (!v) return;
        printf("@keys ds_key=%s bl_key=%s wifi=%s net=%s\n",
               settings_get_key(KEY_DEEPSEEK, v, CMD_LINE_MAX) ? "set" : "unset",
               settings_get_key(KEY_BAILIAN, v, CMD_LINE_MAX) ? "set" : "unset",
               settings_get_wifi(v, 33, pass, sizeof(pass)) ? "set" : "unset",
               net_up() ? "up" : "down");
        wipe(v, CMD_LINE_MAX);
        wipe(pass, sizeof(pass));
        free(v);
    } else if (!strcmp(line, "prompt_begin")) {
        if (!s_prompt) s_prompt = heap_caps_malloc(PROMPT_MAX, MALLOC_CAP_SPIRAM);
        s_prompt_len = 0;
        s_prompt_bad = !s_prompt;
        printf("@ok prompt_begin\n");
    } else if (!strncmp(line, "prompt+", 7)) {
        int n = s_prompt && !s_prompt_bad ? b64_decode(line + 7, s_prompt + s_prompt_len, PROMPT_MAX - s_prompt_len) : -1;
        if (n < 0) s_prompt_bad = true;
        else s_prompt_len += n;
        printf("@%s prompt+ %d\n", n < 0 ? "fail" : "ok", (int)s_prompt_len);
    } else if (!strcmp(line, "prompt_end")) {
        bool ok = !s_prompt_bad && s_prompt_len && prompt_save(s_prompt, s_prompt_len);
        printf("@%s prompt_end %d\n", ok ? "ok" : "fail", (int)s_prompt_len);
    } else if (!strcmp(line, "prompt_clear")) {
        printf("@%s prompt_clear\n", prompt_save(NULL, 0) ? "ok" : "fail");
    } else if (!strcmp(line, "prompt?")) {
        char *p = prompt_load();
        printf("@prompt %d bytes\n", p ? (int)strlen(p) : 0);
        free(p);
    } else if (!strncmp(line, "ask=", 4)) {   /* a typed question, as if spoken */
        ui_chat_inject(line + 4);
        printf("@ok ask\n");
    } else if (!strcmp(line, "hist_dump")) {   /* one "@h <base64 line>" each, then "@h_end" */
        s_dump_n = 0;
        history_dump(dump_line);
        printf("@h_end %d\n", s_dump_n);
    } else if (!strcmp(line, "screen=chat") || !strcmp(line, "screen=home")) {   /* for testing over USB */
        ui_lock();
        if (line[7] == 'c') ui_chat();
        else ui_home();
        ui_unlock();
        printf("@ok screen\n");
    } else if (!strcmp(line, "reboot")) {
        esp_restart();
    }
}

static void console_task(void *arg)
{
    char *line = heap_caps_malloc(CMD_LINE_MAX, MALLOC_CAP_SPIRAM);
    size_t n = 0;
    for (;;) {
        uint8_t c;
        if (usb_serial_jtag_read_bytes(&c, 1, portMAX_DELAY) != 1) {
            continue;
        }
        if (c == '\r' || c == '\n') {
            line[n] = '\0';
            if (n) handle(line);
            wipe(line, CMD_LINE_MAX);
            n = 0;
        } else if (n < CMD_LINE_MAX - 1) {
            line[n++] = (char)c;
        }
    }
}

void console_start(void)
{
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    cfg.rx_buffer_size = 1024;
    if (usb_serial_jtag_driver_install(&cfg) != ESP_OK) {
        ESP_LOGW(TAG, "no USB serial driver; setup commands unavailable");
        return;
    }
    xTaskCreate(console_task, "console", 4096, NULL, 3, NULL);
}
