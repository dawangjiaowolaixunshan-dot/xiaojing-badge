/*
 * 设置: brightness, volume, reading aloud and its voice, when the screen
 * turns off, then battery and network, restart and power off. One column
 * that scrolls; changes apply at once and are saved.
 */
#include <stdio.h>
#include <string.h>

#include "bsp/display.h"
#include "chat.h"
#include "mic.h"
#include "voices.h"
#include "wifi_prov.h"
#include "esp_system.h"
#include "esp_netif.h"
#include "net.h"
#include "pmu.h"
#include "prefs.h"
#include "settings.h"
#include "tts.h"
#include "ui.h"

#define COL_W 300

static lv_obj_t *s_col, *s_cur, *s_sleep_btn_lbl;   /* s_cur: the card being filled */

/* A rounded card holding one group of settings, titled. */
static lv_obj_t *card(const char *title)
{
    s_cur = lv_obj_create(s_col);
    lv_obj_remove_style_all(s_cur);
    lv_obj_set_size(s_cur, COL_W, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(s_cur, lv_color_hex(0x0e1320), 0);
    lv_obj_set_style_bg_opa(s_cur, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_cur, lv_color_hex(0x1d2638), 0);
    lv_obj_set_style_border_width(s_cur, 1, 0);
    lv_obj_set_style_radius(s_cur, 20, 0);
    lv_obj_set_style_pad_all(s_cur, 12, 0);
    lv_obj_set_style_pad_row(s_cur, 6, 0);
    lv_obj_set_flex_flow(s_cur, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_cur, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(s_cur, LV_OBJ_FLAG_SCROLLABLE);
    if (title) ui_label(s_cur, &g_font_text, 0x8fa3c8, title);
    return s_cur;
}

static lv_obj_t *row_title(const char *text)
{
    lv_obj_t *l = ui_label(s_cur, &g_font_text, C_DIM, text);
    lv_obj_set_style_margin_top(l, 4, 0);
    return l;
}

static lv_obj_t *slider(int value, int min, lv_event_cb_t cb)
{
    lv_obj_t *s = lv_slider_create(s_cur);
    lv_obj_set_width(s, COL_W - 60);
    lv_obj_set_height(s, 14);
    lv_slider_set_range(s, min, 100);
    lv_slider_set_value(s, value, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s, lv_color_hex(0x2a2a32), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s, lv_color_hex(C_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s, lv_color_hex(0xffffff), LV_PART_KNOB);
    lv_obj_set_style_pad_all(s, 6, LV_PART_KNOB);
    lv_obj_set_style_margin_ver(s, 8, 0);
    lv_obj_add_event_cb(s, cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s, cb, LV_EVENT_RELEASED, NULL);
    return s;
}

static void on_brightness(lv_event_t *e)
{
    g_prefs.brightness = lv_slider_get_value(lv_event_get_target(e));
    bsp_display_brightness_set(g_prefs.brightness);
    if (lv_event_get_code(e) == LV_EVENT_RELEASED) prefs_save();
}

static void on_volume(lv_event_t *e)
{
    g_prefs.volume = lv_slider_get_value(lv_event_get_target(e));
    tts_set_volume(g_prefs.volume);
    if (lv_event_get_code(e) == LV_EVENT_RELEASED) prefs_save();
}

static void on_read(lv_event_t *e)
{
    tts_set_enabled(lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED));
}

/* ---- choosing a voice: a list, a tap picks one and plays it ---- */

static uint8_t *s_pick;   /* &g_prefs.voice or &g_prefs.poem_voice */

static void on_pick(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    *s_pick = (uint8_t)i;
    prefs_save();
    if (s_pick == &g_prefs.voice) tts_set_voice(PREFS_VOICES[i]);
    lv_obj_t *list = lv_obj_get_parent(lv_event_get_target(e));
    for (uint32_t k = 0; k < lv_obj_get_child_count(list); k++) {
        lv_obj_t *c = lv_obj_get_child(list, k);
        if (lv_obj_check_type(c, &lv_button_class)) {
            lv_obj_set_style_bg_color(c, lv_color_hex(c == lv_event_get_target(e) ? C_BLUE : C_PANEL), 0);
        }
    }
    tts_say_with(PREFS_VOICES[i], s_pick == &g_prefs.voice ? "你好呀，我是小鲸，以后就用这个声音跟你聊天。"
                                                            : "床前明月光，疑是地上霜。举头望明月，低头思故乡。");
}

static void open_picker(uint8_t *which, const char *title)
{
    s_pick = which;
    lv_obj_t *scr = ui_screen();
    ui_back_button(scr);
    lv_obj_t *list = lv_obj_create(scr);
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, 300, 466);
    lv_obj_center(list);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_top(list, 60, 0);
    lv_obj_set_style_pad_bottom(list, 100, 0);
    lv_obj_set_style_pad_row(list, 8, 0);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
    ui_label(list, &g_font_text, C_ACCENT, title);
    ui_label(list, &g_font_text, C_DIM, "点一个就选中，并试听");
    lv_obj_t *mine = NULL;
    for (int i = 0; i < PREFS_VOICE_N; i++) {
        lv_obj_t *b = ui_button(list, PREFS_VOICE_NAMES[i], &g_font_text, 270, 50, i == *which ? C_BLUE : C_PANEL,
                                on_pick, (void *)(intptr_t)i);
        if (i == *which) mine = b;
    }
    ui_show(scr, ui_settings, NULL);
    if (mine) lv_obj_scroll_to_view(mine, LV_ANIM_OFF);
}

static void on_chat_voice(lv_event_t *e) { open_picker(&g_prefs.voice, "小鲸聊天的音色"); }
static void on_poem_voice(lv_event_t *e) { open_picker(&g_prefs.poem_voice, "古诗朗读的音色"); }

static void on_sleep(lv_event_t *e)
{
    g_prefs.sleep = (g_prefs.sleep + 1) % PREFS_SLEEP_N;
    lv_label_set_text(s_sleep_btn_lbl, PREFS_SLEEP_NAMES[g_prefs.sleep]);
    prefs_save();
}

static void on_restart(lv_event_t *e) { esp_restart(); }

/* ---- Wi-Fi: forget (tap twice), add from a phone ---- */

static void on_forget_wifi(lv_event_t *e)
{
    lv_obj_t *b = lv_event_get_target(e);
    lv_obj_t *l = lv_obj_get_child(b, 0);
    if (strcmp(lv_label_get_text(l), "再点一次删除")) {   /* first tap: ask */
        lv_label_set_text(l, "再点一次删除");
        lv_obj_set_style_bg_color(b, lv_color_hex(C_RED), 0);
        return;
    }
    int slot = (int)(intptr_t)lv_event_get_user_data(e);
    settings_forget_wifi(slot);
    if (slot == net_slot()) net_reconnect();
    ui_settings();
}

static lv_obj_t *s_prov_lbl;

static void prov_status(const char *text, bool done)
{
    ui_lock();
    if (s_prov_lbl) lv_label_set_text(s_prov_lbl, text);
    ui_unlock();
}

static void prov_timer(lv_timer_t *t)
{
    static int up_for;
    if (!s_prov_lbl) {
        up_for = 0;
        return;
    }
    const char *now = lv_label_get_text(s_prov_lbl);
    if (strstr(now, "正在连接") && net_up()) {
        lv_label_set_text(s_prov_lbl, "连上了！");
        up_for = 1;
    } else if (up_for && ++up_for > 4) {   /* a moment to read it, then back */
        up_for = 0;
        s_prov_lbl = NULL;
        ui_settings();
    }
}

static void prov_back(void)
{
    s_prov_lbl = NULL;
    prov_stop();
    ui_settings();
}

static void on_add_wifi(lv_event_t *e)
{
    lv_obj_t *scr = ui_screen();
    ui_back_button(scr);
    lv_obj_t *t = ui_label(scr, &g_font_text, C_ACCENT, "添加 Wi-Fi");
    lv_obj_align(t, LV_ALIGN_CENTER, 0, -190);
    s_prov_lbl = ui_label(scr, &g_font_text, C_GREEN, "正在准备…");
    lv_obj_set_width(s_prov_lbl, 330);
    lv_label_set_long_mode(s_prov_lbl, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(s_prov_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_prov_lbl, LV_ALIGN_CENTER, 0, 170);
    ui_show(scr, prov_back, NULL);
    lv_refr_now(NULL);
    if (!prov_start(prov_status)) {
        lv_label_set_text(s_prov_lbl, "开不了热点，稍后再试");
        return;
    }
    char qr[96], buf[160];
    snprintf(qr, sizeof(qr), "WIFI:T:WPA;S:%s;P:%s;;", prov_ap_ssid(), prov_ap_pass());
    lv_obj_t *code = lv_qrcode_create(scr);
    lv_qrcode_set_size(code, 180);
    lv_qrcode_set_dark_color(code, lv_color_black());
    lv_qrcode_set_light_color(code, lv_color_white());
    lv_qrcode_update(code, qr, strlen(qr));
    lv_obj_set_style_border_color(code, lv_color_white(), 0);
    lv_obj_set_style_border_width(code, 8, 0);
    lv_obj_align(code, LV_ALIGN_CENTER, 0, -40);
    memset(qr, 0, sizeof(qr));
    snprintf(buf, sizeof(buf), "手机扫码连上吧唧，会弹出设置页\nWi-Fi：%s  密码：%s", prov_ap_ssid(), prov_ap_pass());
    lv_obj_t *h = ui_label(scr, &g_font_text, C_TEXT, buf);
    lv_obj_set_style_text_align(h, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(h, LV_ALIGN_CENTER, 0, 92);
    lv_label_set_text(s_prov_lbl, "没弹出来就打开 192.168.4.1");
    static lv_timer_t *timer;
    if (!timer) timer = lv_timer_create(prov_timer, 500, NULL);
}

/* ---- 认识声音: each of the family says a line once ---- */

static voice_who_t s_enroll_who;
static lv_obj_t *s_enroll_lbl;

static void no_sink(const int16_t *pcm, size_t n) {}

static void enroll_talk(bool pressed)
{
    if (!s_enroll_lbl) return;
    if (pressed) {
        mic_pitch_begin();
        mic_start(no_sink);
        lv_label_set_text(s_enroll_lbl, "正在听…说完松开");
        return;
    }
    mic_stop();
    int hz = mic_pitch_end();
    char buf[160];
    if (!hz) {
        snprintf(buf, sizeof(buf), "没听清，按住 PWR 再说一句长一点的话");
    } else {
        voices_enroll(s_enroll_who, hz);
        int n = snprintf(buf, sizeof(buf), "记住啦！%s的声音大约 %d Hz", voices_name(s_enroll_who), hz);
        for (int i = 0; i < VOICE_COUNT; i++) {
            int other = voices_enrolled_hz(i);
            if (i == (int)s_enroll_who || !other) continue;
            float r = (float)hz / other;
            if (r > 0.88f && r < 1.14f) {   /* within two semitones or so */
                n += snprintf(buf + n, sizeof(buf) - n, "\n和%s（%d Hz）很接近，可能会分不清", voices_name(i), other);
            }
        }
    }
    lv_label_set_text(s_enroll_lbl, buf);
}

static void enroll_back(void) { s_enroll_lbl = NULL; ui_settings(); }

static void open_enroll(voice_who_t who)
{
    s_enroll_who = who;
    lv_obj_t *scr = ui_screen();
    ui_back_button(scr);
    char buf[160];
    snprintf(buf, sizeof(buf), "录入%s的声音", voices_name(who));
    lv_obj_t *t = ui_label(scr, &g_font_text, C_ACCENT, buf);
    lv_obj_align(t, LV_ALIGN_CENTER, 0, -150);
    snprintf(buf, sizeof(buf), "请%s按住侧面的 PWR 键，\n说一句话，比如\n“小鲸你好，我是%s”，\n说完松开。",
             who == VOICE_CHILD ? "孩子" : who == VOICE_DAD ? "爸爸" : "妈妈", who == VOICE_CHILD ? "小主人" : voices_name(who));
    lv_obj_t *h = ui_label(scr, &g_font_text, C_TEXT, buf);
    lv_obj_set_style_text_align(h, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(h, LV_ALIGN_CENTER, 0, -40);
    s_enroll_lbl = ui_label(scr, &g_font_text, C_GREEN, "");
    lv_obj_set_width(s_enroll_lbl, 320);
    lv_label_set_long_mode(s_enroll_lbl, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(s_enroll_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_enroll_lbl, LV_ALIGN_CENTER, 0, 100);
    ui_show(scr, enroll_back, enroll_talk);
}

static void on_enroll(lv_event_t *e) { open_enroll((voice_who_t)(intptr_t)lv_event_get_user_data(e)); }

static void on_new_chat(lv_event_t *e)
{
    chat_reset();   /* 小鲸 forgets this conversation; the record stays */
    lv_label_set_text(lv_obj_get_child(lv_event_get_target(e), 0), "已开始新对话");
}
static void on_power_off(lv_event_t *e) { pmu_power_off(); }

static lv_obj_t *pill_ud(const char *text, uint32_t bg, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *b = ui_button(s_cur, NULL, &g_font_text, 240, 46, bg, cb, ud);
    lv_obj_set_style_margin_ver(b, 4, 0);
    lv_obj_t *l = ui_label(b, &g_font_text, 0xffffff, text);
    lv_obj_center(l);
    return b;
}

static lv_obj_t *pill(const char *text, uint32_t bg, lv_event_cb_t cb, lv_obj_t **label)
{
    lv_obj_t *b = ui_button(s_cur, NULL, &g_font_text, 240, 46, bg, cb, NULL);
    lv_obj_set_style_margin_ver(b, 4, 0);
    lv_obj_t *l = ui_label(b, &g_font_text, 0xffffff, text);
    lv_obj_center(l);
    if (label) *label = l;
    return b;
}

void ui_settings(void)
{
    char buf[96];
    lv_obj_t *scr = ui_screen();
    ui_back_button(scr);

    s_col = lv_obj_create(scr);
    lv_obj_remove_style_all(s_col);
    lv_obj_set_size(s_col, COL_W, 466);
    lv_obj_center(s_col);
    lv_obj_set_flex_flow(s_col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_top(s_col, 60, 0);
    lv_obj_set_style_pad_bottom(s_col, 90, 0);
    lv_obj_set_style_pad_row(s_col, 10, 0);
    lv_obj_set_scroll_dir(s_col, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_col, LV_SCROLLBAR_MODE_OFF);

    ui_label(s_col, &g_font_poem, C_ACCENT, "设置");

    int pct;
    bool charging, usb;
    pmu_read(&pct, &charging, &usb);
    if (pct >= 0) snprintf(buf, sizeof(buf), "电量 %d%%%s", pct, charging ? "，充电中" : usb ? "，已插电" : "");
    else snprintf(buf, sizeof(buf), "%s", usb ? "USB 供电" : "电量未知");
    card(NULL);
    ui_label(s_cur, &g_font_text, C_TEXT, buf);

    card("屏幕和声音");
    row_title("亮度");
    slider(g_prefs.brightness, 10, on_brightness);
    row_title("音量");
    slider(g_prefs.volume, 0, on_volume);

    card("说话");
    pill(LV_SYMBOL_REFRESH "  开始新对话", C_PANEL, on_new_chat, NULL);
    row_title("朗读回答");
    lv_obj_t *sw = lv_switch_create(s_cur);
    lv_obj_set_style_bg_color(sw, lv_color_hex(C_GREEN), LV_PART_INDICATOR | LV_STATE_CHECKED);
    if (tts_enabled()) lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw, on_read, LV_EVENT_VALUE_CHANGED, NULL);

    row_title("小鲸聊天音色");
    pill(PREFS_VOICE_NAMES[g_prefs.voice], C_BLUE, on_chat_voice, NULL);
    row_title("古诗朗读音色");
    pill(PREFS_VOICE_NAMES[g_prefs.poem_voice], C_ACCENT, on_poem_voice, NULL);

    card("认识声音");
    row_title("每人按住 PWR 说一句，小鲸就分得清谁在说话");
    for (int i = 0; i < VOICE_COUNT; i++) {
        int hz = voices_enrolled_hz(i);
        if (hz) snprintf(buf, sizeof(buf), "%s  已录（%d Hz）", voices_name(i), hz);
        else snprintf(buf, sizeof(buf), "录入%s的声音", voices_name(i));
        pill_ud(buf, hz ? C_PANEL : 0x2f5bea, on_enroll, (void *)(intptr_t)i);
    }

    card("省电");
    row_title("自动熄屏（点一下换）");
    pill(PREFS_SLEEP_NAMES[g_prefs.sleep], C_PANEL, on_sleep, &s_sleep_btn_lbl);

    card("Wi-Fi");
    char ssid[33] = "", pass[65];
    if (net_slot() >= 0) settings_get_wifi(net_slot(), ssid, sizeof(ssid), pass, sizeof(pass));
    memset(pass, 0, sizeof(pass));
    if (net_up()) {
        snprintf(buf, sizeof(buf), "已连上：%s\n信号 %d", ssid, net_rssi());
    } else {
        snprintf(buf, sizeof(buf), "没有连上网络");
    }
    lv_obj_t *n = ui_label(s_cur, &g_font_text, C_TEXT, buf);
    lv_obj_set_style_text_align(n, LV_TEXT_ALIGN_CENTER, 0);
    pill_ud(LV_SYMBOL_PLUS "  添加 Wi-Fi", 0x2f5bea, on_add_wifi, NULL);
    bool any = false;
    for (int i = 0; i < WIFI_SLOTS; i++) {
        char sname[33];
        if (!settings_get_wifi(i, sname, sizeof(sname), pass, sizeof(pass))) continue;
        memset(pass, 0, sizeof(pass));
        if (!any) row_title("记住的网络（点一下删除）");
        any = true;
        snprintf(buf, sizeof(buf), "%s  " LV_SYMBOL_CLOSE, sname);
        pill_ud(buf, C_PANEL, on_forget_wifi, (void *)(intptr_t)i);
    }

    card(NULL);
    pill("重启", C_PANEL, on_restart, NULL);
    pill("关机", C_RED, on_power_off, NULL);

    ui_show(scr, ui_home, NULL);
}

void ui_wifi_add(void) { on_add_wifi(NULL); }
