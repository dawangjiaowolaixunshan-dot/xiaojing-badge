#include "console.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_timer.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "history.h"
#include "net.h"
#include "pet.h"
#include "power.h"
#include "prefs.h"
#include "tts.h"
#include "settings.h"
#include "ui.h"
#include "voices.h"
#include "mic.h"

static const char *TAG = "console";
#define CMD_LINE_MAX 512

static void vp_sink(const int16_t *pcm, size_t n) {}

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

static void power_line(const char *line) { printf("@pw %s\n", line); vTaskDelay(1); }

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
    } else if (!strncmp(line, "wifi=", 5) || !strncmp(line, "wifi2=", 6)) {
        int slot = line[4] == '2';
        const char *tag = slot ? "wifi2" : "wifi";
        char *ssid = line + (slot ? 6 : 5), *tab = strchr(ssid, '\t');
        if (!tab) {
            printf("@fail %s (ssid<TAB>password)\n", tag);
            return;
        }
        *tab = '\0';
        bool ok = settings_set_wifi(slot, ssid, tab + 1);
        wipe(tab + 1, strlen(tab + 1));
        printf("@%s %s\n", ok ? "ok" : "fail", tag);
        if (ok) {
            net_join(slot);
        }
    } else if (!strncmp(line, "wifi_add=", 9)) {
        char *ssid = line + 9, *tab = strchr(ssid, '\t');
        if (!tab) {
            printf("@fail wifi_add (ssid<TAB>password)\n");
            return;
        }
        *tab = '\0';
        int slot = settings_add_wifi(ssid, tab + 1);
        wipe(tab + 1, strlen(tab + 1));
        printf("@%s wifi_add slot=%d\n", slot >= 0 ? "ok" : "fail", slot);
        if (slot >= 0) net_join(slot);
    } else if (!strncmp(line, "wifi_del=", 9)) {
        int slot = atoi(line + 9);
        if (slot >= 0 && slot < WIFI_SLOTS) {
            settings_forget_wifi(slot);
            if (slot == net_slot()) net_reconnect();
            printf("@ok wifi_del %d\n", slot);
        } else {
            printf("@fail wifi_del\n");
        }
    } else if (!strcmp(line, "wifi_list")) {
        char ssid[33], pass[65], st[128];
        int cnt = 0;
        for (int i = 0; i < WIFI_SLOTS; i++) {
            if (settings_get_wifi(i, ssid, sizeof(ssid), pass, sizeof(pass))) {
                printf("@wifi %d %s%s\n", i, ssid, (net_up() && net_slot() == i) ? " [connected]" : "");
                cnt++;
            }
        }
        wipe(pass, sizeof(pass));
        net_status_text(st, sizeof(st));
        for (char *p = st; *p; p++) if (*p == '\n') *p = ' ';
        printf("@wifi_end %d active=%d state=%d reason=%d status=%s\n", cnt, net_slot(), (int)net_state(),
               net_last_reason(), st);
    } else if (!strcmp(line, "wifi_scan")) {
        net_ap_t *aps = heap_caps_calloc(16, sizeof(net_ap_t), MALLOC_CAP_SPIRAM);
        int n = aps ? net_scan(aps, 16) : 0;
        for (int i = 0; i < n; i++) {
            printf("@ap %d %s %d %s\n", (int)aps[i].rssi, aps[i].secured ? "sec" : "open", (int)aps[i].saved_slot,
                   aps[i].ssid);
        }
        free(aps);
        printf("@ap_end %d\n", n);
    } else if (!strcmp(line, "keys?")) {
        char *v = heap_caps_malloc(CMD_LINE_MAX, MALLOC_CAP_SPIRAM);
        char pass[65];
        if (!v) return;
        int wcnt = 0;
        for (int i = 0; i < WIFI_SLOTS; i++) {
            if (settings_get_wifi(i, v, 33, pass, sizeof(pass))) wcnt++;
        }
        printf("@keys ds_key=%s bl_key=%s wifi=%s wifi2=%s wifi_n=%d net=%s\n",
               settings_get_key(KEY_DEEPSEEK, v, CMD_LINE_MAX) ? "set" : "unset",
               settings_get_key(KEY_BAILIAN, v, CMD_LINE_MAX) ? "set" : "unset",
               settings_get_wifi(0, v, 33, pass, sizeof(pass)) ? "set" : "unset",
               settings_get_wifi(1, v, 33, pass, sizeof(pass)) ? "set" : "unset",
               wcnt,
               !net_up() ? "down" : "up");
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
    } else if (!strncmp(line, "vp=", 3)) {   /* "vp=<secs>" hears and names who; "vp=<secs>,child|dad|mom" enrolls a sentence */
                int secs = atoi(line + 3);
        const char *who_s = strchr(line, ',');
        voice_who_t who = !who_s ? VOICE_UNKNOWN : !strcmp(who_s + 1, "child") ? VOICE_CHILD :
                          !strcmp(who_s + 1, "dad") ? VOICE_DAD : !strcmp(who_s + 1, "mom") ? VOICE_MOM : VOICE_UNKNOWN;
        if (who_s && who == VOICE_UNKNOWN) { printf("@fail vp who\n"); return; }
        mic_voice_begin();
        mic_start(vp_sink);
        vTaskDelay(pdMS_TO_TICKS(1000 * (secs > 0 && secs < 10 ? secs : 3)));
        mic_stop();
        voice_sample_t vs;
        bool ok = mic_voice_end(&vs);
        if (!ok) printf("@vp short %d Hz %d blocks\n", vs.hz, vs.frames);
        else if (who != VOICE_UNKNOWN) { voices_enroll(who, &vs); printf("@vp enrolled %s n=%d %d Hz\n", voices_key(who), voices_enrolled_n(who), vs.hz); }
        else { voice_who_t got = voices_classify(&vs); printf("@vp heard %s %d Hz\n", got == VOICE_UNKNOWN ? "unknown" : voices_key(got), vs.hz); }
    } else if (!strncmp(line, "vpreset=", 8)) {
        for (int i = 0; i < VOICE_COUNT; i++) if (!strcmp(line + 8, voices_key(i))) voices_reset(i);
        printf("@ok vpreset\n");
    } else if (!strcmp(line, "scrolltest")) {   /* the home dock swiped both ways; frame times while it moves */
        for (int i = 0; i < 2; i++) {
            ui_lock();
            ui_frames_reset();
            ui_home_scroll_test();
            ui_unlock();
            vTaskDelay(pdMS_TO_TICKS(700));
            int n, avg, mx;
            ui_frames_get(&n, &avg, &mx);
            printf("@scroll %d frames, avg %d ms, max %d ms\n", n, avg / 1000, mx / 1000);
        }
    } else if (!strcmp(line, "voicelog")) {   /* /hist/voice.csv as "@v <line>", for tuning the voice ID */
        FILE *f = fopen("/hist/voice.csv", "r");
        char row[256];
        int n = 0;
        while (f && fgets(row, sizeof(row), f)) {
            row[strcspn(row, "\n")] = 0;
            printf("@v %s\n", row);
            if (++n % 20 == 0) vTaskDelay(1);
        }
        if (f) fclose(f);
        printf("@v_end %d\n", n);
    } else if (!strcmp(line, "hist_dump")) {   /* one "@h <base64 line>" each, then "@h_end" */
        s_dump_n = 0;
        history_dump(dump_line);
        printf("@h_end %d\n", s_dump_n);
    } else if (!strncmp(line, "screen=", 7)) {   /* for testing over USB */
        ui_lock();
        if (!strcmp(line + 7, "chat")) ui_chat();
        else if (!strcmp(line + 7, "settings")) ui_settings();
        else if (!strcmp(line + 7, "poem")) ui_poem_home();
        else if (!strcmp(line + 7, "recite")) ui_recite();
        else if (!strcmp(line + 7, "wifiadd")) ui_wifi_add();
        else if (!strcmp(line + 7, "wifiscan")) ui_wifi_scan();
        else if (!strcmp(line + 7, "wifipass")) ui_wifi_pass_test("iPhone 热点");
        else if (!strcmp(line + 7, "words")) ui_words();
        else if (!strcmp(line + 7, "math")) ui_math();
        else ui_home();
        ui_unlock();
        printf("@ok screen\n");
    } else if (!strncmp(line, "chatvoice=", 10)) {   /* pick 小鲸's voice by index (as in 设置) */
        int i = atoi(line + 10);
        if (i >= 0 && i < PREFS_VOICE_N) {
            g_prefs.voice = (uint8_t)i;
            prefs_save();
            tts_set_voice(PREFS_VOICES[i]);
        }
        printf("@ok chatvoice %d\n", i);
    } else if (!strncmp(line, "poemvoice=", 10)) {   /* pick poem voice by index */
        int i = atoi(line + 10);
        if (i >= 0 && i < PREFS_VOICE_N) {
            g_prefs.poem_voice = (uint8_t)i;
            prefs_save();
        }
        printf("@ok poemvoice %d\n", i);
    } else if (!strncmp(line, "voicetest=", 10)) {   /* speak a line in voice N, to check the id works */
        int i = atoi(line + 10);
        if (i >= 0 && i < PREFS_VOICE_N) tts_say_with(PREFS_VOICES[i], "你好");
        printf("@ok voicetest %d\n", i);
    } else if (!strcmp(line, "shot")) {   /* the screen as RGB565, base64, for tools/badge_shot.py */
        ui_lock();
        lv_draw_buf_t *snap = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_RGB565);
        ui_unlock();
        if (!snap) {
            printf("@shot_fail\n");
            return;
        }
        int w = snap->header.w, h = snap->header.h, stride = snap->header.stride;
        printf("@shot %d %d\n", w, h);
        static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        static char out[1300];   /* a 466-px row: 932 bytes, 1244 in base64 */
        for (int y = 0; y < h; y++) {
            const uint8_t *row = snap->data + y * stride;
            int n = w * 2, o = 0;
            for (int i = 0; i < n; i += 3) {
                uint32_t v = row[i] << 16 | (i + 1 < n ? row[i + 1] << 8 : 0) | (i + 2 < n ? row[i + 2] : 0);
                out[o++] = T[v >> 18 & 63];
                out[o++] = T[v >> 12 & 63];
                out[o++] = i + 1 < n ? T[v >> 6 & 63] : '=';
                out[o++] = i + 2 < n ? T[v & 63] : '=';
                if (o > (int)sizeof(out) - 8) break;
            }
            out[o] = 0;
            printf("@r %d %s\n", y, out);
            if (y % 8 == 0) vTaskDelay(1);
        }
        printf("@shot_end\n");
        lv_draw_buf_destroy(snap);
    } else if (!strncmp(line, "math=", 5)) {   /* as if said aloud; "math=!" says the right answer */
        ui_lock();
        char buf[16];
        if (!strcmp(line + 5, "!")) snprintf(buf, sizeof(buf), "%d", ui_math_answer());
        ui_math_inject(!strcmp(line + 5, "!") ? buf : line + 5);
        ui_unlock();
        printf("@ok math\n");
    } else if (!strncmp(line, "word=", 5)) {   /* as if read aloud */
        ui_lock();
        ui_words_inject(line + 5);
        ui_unlock();
        printf("@ok word\n");
    } else if (!strncmp(line, "recite=", 7)) {
        ui_lock();
        ui_recite_inject(line + 7);
        ui_unlock();
        printf("@ok recite\n");
    } else if (!strcmp(line, "poemread")) {
        ui_lock();
        ui_poem_read();
        ui_unlock();
    } else if (!strncmp(line, "sleepidx=", 9) || !strcmp(line, "prefs?")) {   /* auto screen-off: 0 never,1 30s,2 1m,3 3m */
        if (line[0] == 's') {
            int i = atoi(line + 9);
            if (i >= 0 && i < PREFS_SLEEP_N) {
                g_prefs.sleep = (uint8_t)i;
                prefs_save();
            }
        }
        printf("@prefs bright=%d vol=%d sleep=%d voice=%d poem=%d\n", g_prefs.brightness, g_prefs.volume,
               g_prefs.sleep, g_prefs.voice, g_prefs.poem_voice);
    } else if (!strncmp(line, "wake", 4)) {   /* as if touched */
        ui_lock();
        lv_display_trigger_activity(NULL);
        ui_unlock();
        printf("@ok wake\n");
    } else if (!strncmp(line, "vol=", 4)) {
        int v = atoi(line + 4);
        if (ui_lock()) {
            ui_volume_apply(v, true);
            ui_unlock();
        }
        printf("@ok vol %d\n", (int)g_prefs.volume);
    } else if (!strcmp(line, "volui")) {
        if (ui_lock()) {
            if (ui_volume_is_open()) ui_volume_close();
            else ui_volume_popup();
            ui_unlock();
        }
        printf("@ok volui\n");
    } else if (!strncmp(line, "swipepreview=", 13)) {
        if (ui_lock()) {
            ui_swipe_preview(atoi(line + 13));
            ui_unlock();
        }
        printf("@ok swipepreview\n");
    } else if (!strcmp(line, "powerdump")) {   /* "@pw <line>" each, then "@pw_end" */
        power_dump(power_line);
        printf("@pw_end\n");
    } else if (!strcmp(line, "petdbg")) {
        ui_lock();
        pet_debug();
        ui_unlock();
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

static bool s_driver;

/*
 * The USB serial driver goes in first thing in app_main, before Wi-Fi, the
 * display and the other tasks: installed late, its interrupt allocation hit
 * the interrupt watchdog at boot (a boot loop), with everything else already
 * running and logging.
 */
void console_init_early(void)
{
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    cfg.rx_buffer_size = 1024;
    cfg.tx_buffer_size = 4096;
    if (usb_serial_jtag_driver_install(&cfg) != ESP_OK) {
        ESP_LOGW(TAG, "no USB serial driver; setup commands unavailable");
        return;
    }
    /* Logs and printf through the driver too. Left on the default direct
     * path, they fought the driver for the same FIFO: under a big output
     * (a screenshot) the port wedged, and every task that logged hung with
     * it (the chat stopped showing text, the UI stalled). The driver path
     * drops output after 50 ms with no host instead of blocking. */
    usb_serial_jtag_vfs_use_driver();
    s_driver = true;
}

void console_start(void)
{
    if (!s_driver) return;
    xTaskCreate(console_task, "console", 8192, NULL, 3, NULL);   /* >shot takes a snapshot on this stack */
}
