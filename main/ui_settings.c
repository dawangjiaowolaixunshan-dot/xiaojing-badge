/*
 * 设置: brightness, volume, reading aloud and its voice, when the screen
 * turns off, then battery and network, restart and power off. One column
 * that scrolls; changes apply at once and are saved.
 */
#include <stdio.h>
#include <string.h>

#include "chat.h"
#include "mic.h"
#include "voices.h"
#include "wifi_prov.h"
#include "esp_system.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "net.h"
#include "pmu.h"
#include "prefs.h"
#include "settings.h"
#include "tts.h"
#include "ui.h"

#define COL_W 300

static lv_obj_t *s_col, *s_cur, *s_sleep_btn_lbl;   /* s_cur: the card being filled */
static lv_obj_t *s_vp_clear_lbl, *s_vp_btns[VOICE_COUNT];
static lv_obj_t *s_bright_lbl, *s_vol_lbl, *s_vol_slider, *s_vol_mute_lbl, *s_vol_mute_btn;
static int s_set_vol_prev = 75;

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
    ui_set_brightness(g_prefs.brightness);
    if (s_bright_lbl) {
        char buf[24];
        snprintf(buf, sizeof(buf), "亮度 %d%%", g_prefs.brightness);
        lv_label_set_text(s_bright_lbl, buf);
    }
    if (lv_event_get_code(e) == LV_EVENT_RELEASED) prefs_save();
}

void ui_settings_sync_volume(void)
{
    int v = g_prefs.volume;
    if (v > 0) s_set_vol_prev = v;
    if (s_vol_lbl) {
        char buf[32];
        if (v <= 0) snprintf(buf, sizeof(buf), "音量 0%%（已静音）");
        else snprintf(buf, sizeof(buf), "音量 %d%%", v);
        lv_label_set_text(s_vol_lbl, buf);
        lv_obj_set_style_text_color(s_vol_lbl, lv_color_hex(v <= 0 ? C_RED : C_DIM), 0);
    }
    if (s_vol_slider && lv_slider_get_value(s_vol_slider) != v) {
        lv_slider_set_value(s_vol_slider, v, LV_ANIM_OFF);
    }
    if (s_vol_mute_lbl && s_vol_mute_btn) {
        lv_label_set_text(s_vol_mute_lbl, v <= 0 ? "恢复" : "静音");
        lv_obj_set_style_bg_color(s_vol_mute_btn, lv_color_hex(v <= 0 ? 0x26734d : C_PANEL), 0);
    }
}

static void on_volume(lv_event_t *e)
{
    int v = lv_slider_get_value(lv_event_get_target(e));
    ui_volume_apply(v, lv_event_get_code(e) == LV_EVENT_RELEASED);
}

static void on_set_vol_minus(lv_event_t *e)
{
    int v = (int)g_prefs.volume - 10;
    ui_volume_apply(v < 0 ? 0 : v, true);
}

static void on_set_vol_plus(lv_event_t *e)
{
    int v = (int)g_prefs.volume + 10;
    ui_volume_apply(v > 100 ? 100 : v, true);
}

static void on_set_vol_mute(lv_event_t *e)
{
    if (g_prefs.volume > 0) {
        s_set_vol_prev = g_prefs.volume;
        ui_volume_apply(0, true);
    } else {
        ui_volume_apply(s_set_vol_prev > 0 ? s_set_vol_prev : 75, true);
    }
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
    const char *sample =
        s_pick == &g_prefs.voice
            ? "你好呀，听听这个声音顺不顺耳，以后就用它陪你聊天。"
            : "床前明月光，疑是地上霜。举头望明月，低头思故乡。";
    tts_say_with(PREFS_VOICES[i], sample);
}

static void on_picker_delete(lv_event_t *e)
{
    (void)e;
    tts_stop();
}

static void open_picker(uint8_t *which, const char *title)
{
    s_pick = which;
    lv_obj_t *scr = ui_screen();
    lv_obj_add_event_cb(scr, on_picker_delete, LV_EVENT_DELETE, NULL);
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

/* ---- Wi-Fi: pure-STA scan & on-screen password entry, plus fallback QR helper ---- */

#define MAX_SCAN_UI 16
static lv_obj_t *s_settings_scr, *s_settings_net_lbl;
static lv_obj_t *s_scan_scr, *s_scan_list, *s_scan_status;
static lv_obj_t *s_pass_scr, *s_pass_ta;
static lv_obj_t *s_conn_scr, *s_conn_lbl, *s_conn_btns;
static lv_obj_t *s_prov_lbl;

static net_ap_t s_aps[MAX_SCAN_UI];
static int s_naps;
static volatile bool s_scan_running;
static char s_edit_ssid[33];
static int s_edit_saved_slot = -1;
static int s_conn_slot = -1;
static bool s_conn_can_reenter;
static int s_conn_ticks, s_conn_ok_ticks;

bool ui_wifi_busy(void)
{
    return s_scan_scr != NULL || s_pass_scr != NULL || s_conn_scr != NULL || s_prov_lbl != NULL;
}

static void open_wifi_scan(void);
static void open_wifi_pass(const char *ssid, int saved_slot);
static void open_wifi_connecting(const char *ssid, bool can_reenter, int slot);

static void on_forget_wifi(lv_event_t *e)
{
    lv_obj_t *b = lv_event_get_target(e);
    lv_obj_t *l = lv_obj_get_child(b, 0);
    if (strcmp(lv_label_get_text(l), "删?")) {   /* first tap: confirm */
        lv_label_set_text(l, "删?");
        lv_obj_set_style_bg_color(b, lv_color_hex(C_RED), 0);
        return;
    }
    int slot = (int)(intptr_t)lv_event_get_user_data(e);
    settings_forget_wifi(slot);
    if (slot == net_slot()) net_reconnect();
    ui_settings();
}

static void on_connect_saved(lv_event_t *e)
{
    int slot = (int)(intptr_t)lv_event_get_user_data(e);
    char ssid[33] = "", pass[65];
    if (!settings_get_wifi(slot, ssid, sizeof(ssid), pass, sizeof(pass))) return;
    memset(pass, 0, sizeof(pass));
    net_join(slot);
    open_wifi_connecting(ssid, true, slot);
}

/* ---- Connecting progress screen ---- */

static void on_conn_delete(lv_event_t *e)
{
    if (lv_event_get_target(e) != s_conn_scr) return;
    s_conn_scr = s_conn_lbl = s_conn_btns = NULL;
}

static void on_conn_retry(lv_event_t *e)
{
    if (s_conn_slot >= 0) {
        s_conn_ticks = s_conn_ok_ticks = 0;
        if (s_conn_btns) lv_obj_add_flag(s_conn_btns, LV_OBJ_FLAG_HIDDEN);
        if (s_conn_lbl) {
            lv_obj_set_style_text_color(s_conn_lbl, lv_color_hex(C_BLUE), 0);
            lv_label_set_text(s_conn_lbl, "正在重新连接…");
        }
        net_join(s_conn_slot);
    }
}

static void on_conn_repass(lv_event_t *e)
{
    open_wifi_pass(s_edit_ssid, s_conn_slot);
}

static void on_conn_done_btn(lv_event_t *e) { ui_settings(); }

static void wifi_conn_timer(lv_timer_t *t)
{
    if (!s_conn_scr || !s_conn_lbl) return;
    s_conn_ticks++;
    if (net_up() && (s_conn_slot < 0 || net_slot() == s_conn_slot)) {
        if (s_conn_ok_ticks == 0) {
            char buf[96];
            snprintf(buf, sizeof(buf), "连上了！\n信号 %d dBm", net_rssi());
            lv_obj_set_style_text_color(s_conn_lbl, lv_color_hex(C_GREEN), 0);
            lv_label_set_text(s_conn_lbl, buf);
            if (s_conn_btns) lv_obj_add_flag(s_conn_btns, LV_OBJ_FLAG_HIDDEN);
        }
        if (++s_conn_ok_ticks >= 4) {
            ui_settings();
        }
        return;
    }
    if (s_conn_ticks >= 3 && net_state() == NET_STATE_FAILED) {
        char buf[160];
        net_status_text(buf, sizeof(buf));
        lv_obj_set_style_text_color(s_conn_lbl, lv_color_hex(C_RED), 0);
        lv_label_set_text(s_conn_lbl, buf);
        if (s_conn_btns) lv_obj_remove_flag(s_conn_btns, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    static const char *DOTS[] = {"正在连接.", "正在连接..", "正在连接..."};
    lv_obj_set_style_text_color(s_conn_lbl, lv_color_hex(C_BLUE), 0);
    lv_label_set_text(s_conn_lbl, DOTS[s_conn_ticks % 3]);
}

static void open_wifi_connecting(const char *ssid, bool can_reenter, int slot)
{
    strlcpy(s_edit_ssid, ssid ? ssid : "", sizeof(s_edit_ssid));
    s_conn_slot = slot;
    s_conn_can_reenter = can_reenter;
    s_conn_ticks = s_conn_ok_ticks = 0;

    lv_obj_t *scr = ui_screen();
    lv_obj_t *title = ui_label(scr, &g_font_text, C_ACCENT, s_edit_ssid);
    lv_obj_set_width(title, 280);
    lv_label_set_long_mode(title, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -110);

    s_conn_lbl = ui_label(scr, &g_font_text, C_BLUE, "正在连接…");
    lv_obj_set_width(s_conn_lbl, 330);
    lv_label_set_long_mode(s_conn_lbl, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(s_conn_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_conn_lbl, LV_ALIGN_CENTER, 0, -25);

    s_conn_btns = lv_obj_create(scr);
    lv_obj_remove_style_all(s_conn_btns);
    lv_obj_set_size(s_conn_btns, 320, 120);
    lv_obj_set_flex_flow(s_conn_btns, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_conn_btns, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_conn_btns, 10, 0);
    lv_obj_align(s_conn_btns, LV_ALIGN_CENTER, 0, 95);
    lv_obj_add_flag(s_conn_btns, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *row = lv_obj_create(s_conn_btns);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 12, 0);
    if (can_reenter) {
        ui_button(row, "重输密码", &g_font_text, 126, 44, 0x2f5bea, on_conn_repass, NULL);
    }
    ui_button(row, "重试", &g_font_text, 100, 44, C_PANEL, on_conn_retry, NULL);
    ui_button(s_conn_btns, "返回设置", &g_font_text, 140, 40, C_PANEL, on_conn_done_btn, NULL);

    lv_obj_add_event_cb(scr, on_conn_delete, LV_EVENT_DELETE, NULL);
    s_conn_scr = scr;
    static lv_timer_t *timer;
    if (!timer) timer = lv_timer_create(wifi_conn_timer, 400, NULL);
    ui_show(scr, ui_settings, NULL);
}

/* ---- On-screen keyboard password entry (pure STA, no AP hotspot needed) ---- */

static void on_pass_delete(lv_event_t *e)
{
    if (lv_event_get_target(e) != s_pass_scr) return;
    s_pass_scr = s_pass_ta = NULL;
}

static void submit_wifi_pass(void)
{
    if (!s_pass_ta) return;
    const char *pw = lv_textarea_get_text(s_pass_ta);
    int slot = settings_add_wifi(s_edit_ssid, pw ? pw : "");
    if (slot >= 0) {
        net_join(slot);
        open_wifi_connecting(s_edit_ssid, true, slot);
    } else {
        open_wifi_scan();
    }
}

static void on_pass_connect(lv_event_t *e) { submit_wifi_pass(); }
static void on_pass_back(lv_event_t *e) { open_wifi_scan(); }
static void on_pass_clear(lv_event_t *e)
{
    if (s_pass_ta) lv_textarea_set_text(s_pass_ta, "");
}
static void on_pass_use_saved(lv_event_t *e)
{
    if (s_edit_saved_slot >= 0) {
        net_join(s_edit_saved_slot);
        open_wifi_connecting(s_edit_ssid, true, s_edit_saved_slot);
    }
}

static void on_kb_event(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY) submit_wifi_pass();
    else if (code == LV_EVENT_CANCEL) open_wifi_scan();
}

static void open_wifi_pass(const char *ssid, int saved_slot)
{
    strlcpy(s_edit_ssid, ssid ? ssid : "", sizeof(s_edit_ssid));
    s_edit_saved_slot = saved_slot;

    lv_obj_t *scr = ui_screen();
    lv_obj_t *title = ui_label(scr, &g_font_text, C_ACCENT, s_edit_ssid);
    lv_obj_set_width(title, 240);
    lv_label_set_long_mode(title, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -184);

    s_pass_ta = lv_textarea_create(scr);
    lv_obj_set_size(s_pass_ta, 286, 44);
    lv_textarea_set_one_line(s_pass_ta, true);
    lv_textarea_set_max_length(s_pass_ta, 64);
    lv_textarea_set_placeholder_text(s_pass_ta, "输入 Wi-Fi 密码");
    lv_obj_set_style_text_font(s_pass_ta, &g_font_text, 0);
    lv_obj_set_style_bg_color(s_pass_ta, lv_color_hex(0x151c2c), 0);
    lv_obj_set_style_text_color(s_pass_ta, lv_color_white(), 0);
    lv_obj_set_style_border_color(s_pass_ta, lv_color_hex(0x3b4d70), 0);
    lv_obj_set_style_border_width(s_pass_ta, 1, 0);
    lv_obj_set_style_radius(s_pass_ta, 12, 0);
    lv_obj_set_style_pad_ver(s_pass_ta, 6, 0);
    lv_obj_align(s_pass_ta, LV_ALIGN_CENTER, 0, -142);

    lv_obj_t *row = lv_obj_create(scr);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 8, 0);
    lv_obj_align(row, LV_ALIGN_CENTER, 0, -92);
    ui_button(row, "返回", &g_font_text, 74, 36, C_PANEL, on_pass_back, NULL);
    if (saved_slot >= 0) {
        ui_button(row, "旧密码连", &g_font_text, 114, 36, 0x26734d, on_pass_use_saved, NULL);
    } else {
        ui_button(row, "清空", &g_font_text, 74, 36, C_PANEL, on_pass_clear, NULL);
    }
    ui_button(row, "连接", &g_font_text, 88, 36, 0x2f5bea, on_pass_connect, NULL);

    /* 336x208 centered at y=+46 -> corners at (±168, +150), radius 225 <= 233. */
    lv_obj_t *kb = lv_keyboard_create(scr);
    lv_obj_set_size(kb, 336, 208);
    lv_obj_align(kb, LV_ALIGN_CENTER, 0, 46);
    lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_keyboard_set_textarea(kb, s_pass_ta);
    lv_obj_set_style_bg_color(kb, lv_color_hex(0x0b0f19), 0);
    lv_obj_set_style_bg_opa(kb, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(kb, 4, 0);
    lv_obj_set_style_pad_gap(kb, 4, 0);
    lv_obj_set_style_bg_color(kb, lv_color_hex(0x1c2436), LV_PART_ITEMS);
    lv_obj_set_style_bg_color(kb, lv_color_hex(0x2f5bea), LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_text_color(kb, lv_color_white(), LV_PART_ITEMS);
    lv_obj_set_style_text_font(kb, &lv_font_montserrat_20, LV_PART_ITEMS);
    lv_obj_set_style_radius(kb, 8, LV_PART_ITEMS);
    lv_obj_add_event_cb(kb, on_kb_event, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(kb, on_kb_event, LV_EVENT_CANCEL, NULL);

    lv_obj_t *hint = ui_label(scr, &g_font_text, C_DIM, "点 1# 切数字和符号");
    lv_obj_align(hint, LV_ALIGN_CENTER, 0, 174);

    lv_obj_add_event_cb(scr, on_pass_delete, LV_EVENT_DELETE, NULL);
    s_pass_scr = scr;
    ui_show(scr, open_wifi_scan, NULL);
    ui_swipe_edge_only(true);
}

/* ---- Pure-STA nearby Wi-Fi scan screen ---- */

static void on_scan_delete(lv_event_t *e)
{
    if (lv_event_get_target(e) != s_scan_scr) return;
    s_scan_scr = s_scan_list = s_scan_status = NULL;
}

static void on_pick_ap(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= s_naps) return;
    const net_ap_t *ap = &s_aps[idx];
    if (!ap->secured) {
        int slot = settings_add_wifi(ap->ssid, "");
        if (slot >= 0) {
            net_join(slot);
            open_wifi_connecting(ap->ssid, false, slot);
        }
        return;
    }
    open_wifi_pass(ap->ssid, ap->saved_slot);
}

static void populate_scan_list(void)
{
    if (!s_scan_list || !s_scan_status) return;
    lv_obj_clean(s_scan_list);
    if (s_naps == 0) {
        lv_label_set_text(s_scan_status, "未扫到 2.4G 网络\niPhone 热点请开「最大兼容性」");
        return;
    }
    char hdr[64];
    snprintf(hdr, sizeof(hdr), "找到 %d 个网络，点选连接", s_naps);
    lv_label_set_text(s_scan_status, hdr);
    for (int i = 0; i < s_naps; i++) {
        const net_ap_t *ap = &s_aps[i];
        bool connected = net_up() && ap->saved_slot >= 0 && ap->saved_slot == net_slot();
        lv_obj_t *b = ui_button(s_scan_list, NULL, &g_font_text, 286, 52,
                                connected ? 0x1f5c3a : (ap->saved_slot >= 0 ? 0x1a2640 : C_PANEL),
                                on_pick_ap, (void *)(intptr_t)i);
        lv_obj_set_style_radius(b, 14, 0);
        lv_obj_set_style_pad_hor(b, 14, 0);
        lv_obj_t *name = ui_label(b, &g_font_text, 0xffffff, ap->ssid);
        lv_obj_set_width(name, 190);
        lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_align(name, LV_ALIGN_LEFT_MID, 0, 0);

        const char *sig = ap->rssi >= -55 ? "强" : ap->rssi >= -70 ? "中" : "弱";
        char meta[24];
        if (connected) snprintf(meta, sizeof(meta), "已连 %s", sig);
        else if (ap->saved_slot >= 0) snprintf(meta, sizeof(meta), "已存 %s", sig);
        else snprintf(meta, sizeof(meta), "%s%s", ap->secured ? "锁 " : "", sig);
        lv_obj_t *m = ui_label(b, &g_font_text, connected ? C_GREEN : C_DIM, meta);
        lv_obj_align(m, LV_ALIGN_RIGHT_MID, 0, 0);
    }
}

static void wifi_scan_task(void *arg)
{
    net_ap_t *tmp = calloc(MAX_SCAN_UI, sizeof(net_ap_t));
    int n = tmp ? net_scan(tmp, MAX_SCAN_UI) : 0;
    ui_lock();
    if (tmp) {
        memcpy(s_aps, tmp, sizeof(s_aps));
        s_naps = n;
    }
    s_scan_running = false;
    if (s_scan_scr) populate_scan_list();
    ui_unlock();
    free(tmp);
    vTaskDelete(NULL);
}

static void start_wifi_scan(void)
{
    if (s_scan_running) return;
    s_scan_running = true;
    if (s_scan_status) lv_label_set_text(s_scan_status, "正在扫描 2.4G Wi-Fi…");
    if (s_scan_list) lv_obj_clean(s_scan_list);
    if (xTaskCreate(wifi_scan_task, "wscan", 4096, NULL, 3, NULL) != pdPASS) {
        s_scan_running = false;
        if (s_scan_status) lv_label_set_text(s_scan_status, "扫描启动失败，再试一次");
    }
}

static void on_rescan(lv_event_t *e) { start_wifi_scan(); }

static void open_wifi_scan(void)
{
    lv_obj_t *scr = ui_screen();

    lv_obj_t *col = lv_obj_create(scr);
    lv_obj_remove_style_all(col);
    lv_obj_set_size(col, COL_W, 466);
    lv_obj_center(col);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_top(col, 56, 0);
    lv_obj_set_style_pad_bottom(col, 90, 0);
    lv_obj_set_style_pad_row(col, 8, 0);
    lv_obj_set_scroll_dir(col, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(col, LV_SCROLLBAR_MODE_OFF);

    ui_label(col, &g_font_text, C_ACCENT, "附近 Wi-Fi");
    s_scan_status = ui_label(col, &g_font_text, C_DIM, "正在扫描 2.4G Wi-Fi…");
    lv_obj_set_width(s_scan_status, 286);
    lv_label_set_long_mode(s_scan_status, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(s_scan_status, LV_TEXT_ALIGN_CENTER, 0);

    ui_button(col, LV_SYMBOL_REFRESH "  重新扫描", &g_font_text, 200, 42, 0x2f5bea, on_rescan, NULL);

    s_scan_list = lv_obj_create(col);
    lv_obj_remove_style_all(s_scan_list);
    lv_obj_set_size(s_scan_list, COL_W, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_scan_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_scan_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_scan_list, 8, 0);
    lv_obj_remove_flag(s_scan_list, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *tip = ui_label(col, &g_font_text, C_DIM, "仅支持 2.4G 网络\niPhone 热点请开\n「最大兼容性」");
    lv_obj_set_width(tip, 280);
    lv_label_set_long_mode(tip, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(tip, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_margin_top(tip, 6, 0);

    lv_obj_add_event_cb(scr, on_scan_delete, LV_EVENT_DELETE, NULL);
    s_scan_scr = scr;
    ui_show(scr, ui_settings, NULL);
    start_wifi_scan();
}

static void on_scan_wifi(lv_event_t *e) { open_wifi_scan(); }

/* ---- Fallback QR/AP provisioning (optional, for long router passwords) ---- */

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
    } else if (strstr(now, "正在连接") && !prov_active() && net_state() == NET_STATE_FAILED) {
        char buf[160];
        net_status_text(buf, sizeof(buf));
        lv_obj_set_style_text_color(s_prov_lbl, lv_color_hex(C_RED), 0);
        lv_label_set_text(s_prov_lbl, buf);
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

static void on_prov_delete(lv_event_t *e)
{
    s_prov_lbl = NULL;
}

static void on_add_wifi(lv_event_t *e)
{
    lv_obj_t *scr = ui_screen();
    lv_obj_t *t = ui_label(scr, &g_font_text, C_ACCENT, "扫码传密码（备选）");
    lv_obj_align(t, LV_ALIGN_CENTER, 0, -190);
    s_prov_lbl = ui_label(scr, &g_font_text, C_GREEN, "正在准备…");
    lv_obj_set_width(s_prov_lbl, 330);
    lv_label_set_long_mode(s_prov_lbl, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(s_prov_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_prov_lbl, LV_ALIGN_CENTER, 0, 170);
    lv_obj_add_event_cb(scr, on_prov_delete, LV_EVENT_DELETE, NULL);
    ui_show(scr, prov_back, NULL);
    lv_refr_now(NULL);
    if (!prov_start(prov_status)) {
        lv_label_set_text(s_prov_lbl, "开不了临时热点，稍后再试");
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
    snprintf(buf, sizeof(buf), "扫码临时连上吧唧发密码\nWi-Fi：%s  密码：%s", prov_ap_ssid(), prov_ap_pass());
    lv_obj_t *h = ui_label(scr, &g_font_text, C_TEXT, buf);
    lv_obj_set_style_text_align(h, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(h, LV_ALIGN_CENTER, 0, 92);
    lv_label_set_text(s_prov_lbl, "没弹出来就打开 192.168.4.1");
    static lv_timer_t *timer;
    if (!timer) timer = lv_timer_create(prov_timer, 500, NULL);
}

static void settings_net_timer(lv_timer_t *t)
{
    if (!s_settings_scr || !s_settings_net_lbl) return;
    char buf[160];
    net_status_text(buf, sizeof(buf));
    lv_label_set_text(s_settings_net_lbl, buf);
    lv_obj_set_style_text_color(s_settings_net_lbl,
                                lv_color_hex(net_up() ? C_GREEN : (net_state() == NET_STATE_FAILED ? C_RED : C_TEXT)),
                                0);
}

static void on_settings_delete(lv_event_t *e)
{
    if (lv_event_get_target(e) != s_settings_scr) return;
    s_settings_scr = s_settings_net_lbl = s_sleep_btn_lbl = s_vp_clear_lbl = NULL;
    s_bright_lbl = s_vol_lbl = s_vol_slider = s_vol_mute_lbl = s_vol_mute_btn = NULL;
    memset(s_vp_btns, 0, sizeof(s_vp_btns));
}

/* ---- 认识声音: each of the family says three short lines ---- */

static voice_who_t s_enroll_who;
static lv_obj_t *s_enroll_lbl, *s_enroll_line;

static const char *enroll_line(voice_who_t who, int i)
{
    static const char *CHILD[] = {"小鲸你好，我是你的小主人", "今天我想背一首古诗", "三十七加四十八等于八十五"};
    static const char *ADULT[] = {"小鲸你好，我是%s", "今天天气不错，我们出去走走", "给孩子讲一个有趣的故事吧"};
    static char buf[80];
    if (who == VOICE_CHILD) return CHILD[i % 3];
    snprintf(buf, sizeof(buf), ADULT[i % 3], voices_name(who));
    return buf;
}

static void show_enroll_line(void)
{
    int n = voices_enrolled_n(s_enroll_who);
    char buf[160];
    if (n >= VOICE_ENROLL_SENTENCES) {
        snprintf(buf, sizeof(buf), "录好了！\n想再录一遍就退出重进");
    } else {
        snprintf(buf, sizeof(buf), "第 %d / %d 句，按住 PWR 读：\n“%s”", n + 1, VOICE_ENROLL_SENTENCES,
                 enroll_line(s_enroll_who, n));
    }
    lv_label_set_text(s_enroll_line, buf);
}

static void no_sink(const int16_t *pcm, size_t n) {}

static void enroll_talk(bool pressed)
{
    if (!s_enroll_lbl || voices_enrolled_n(s_enroll_who) >= VOICE_ENROLL_SENTENCES) return;
    if (pressed) {
        mic_voice_begin();
        mic_start(no_sink);
        lv_label_set_text(s_enroll_lbl, "正在听…说完松开");
        return;
    }
    mic_stop();
    voice_sample_t vs;
    char buf[200];
    if (!mic_voice_end(&vs)) {
        snprintf(buf, sizeof(buf), "没听清，再读一遍，声音大一点");
    } else {
        voices_enroll(s_enroll_who, &vs);
        int n = snprintf(buf, sizeof(buf), "记住了这一句（%d Hz）", vs.hz);
        if (n < 0) n = 0;
        if ((size_t)n >= sizeof(buf)) n = (int)sizeof(buf) - 1;
        if (voices_enrolled_n(s_enroll_who) >= VOICE_ENROLL_SENTENCES) {
            /* How far the others' voices are from this one, now it's complete. */
            for (int i = 0; i < VOICE_COUNT && (size_t)n < sizeof(buf) - 1; i++) {
                if (i == (int)s_enroll_who || !voices_enrolled_n(i)) continue;
                float sc = voices_score(i, &vs);
                if (sc < VP_UNKNOWN) {
                    int w = snprintf(buf + n, sizeof(buf) - (size_t)n, "\n和%s的声音有点像，可能偶尔认错",
                                     voices_name(i));
                    if (w > 0) {
                        n += w;
                        if ((size_t)n >= sizeof(buf)) n = (int)sizeof(buf) - 1;
                    }
                }
            }
        }
    }
    lv_label_set_text(s_enroll_lbl, buf);
    show_enroll_line();
}

static void enroll_back(void) { s_enroll_lbl = s_enroll_line = NULL; ui_settings(); }

static void open_enroll(voice_who_t who)
{
    s_enroll_who = who;
    voices_reset(who);   /* a fresh recording each time */
    lv_obj_t *scr = ui_screen();
    char buf[160];
    snprintf(buf, sizeof(buf), "录入%s的声音", voices_name(who));
    lv_obj_t *t = ui_label(scr, &g_font_text, C_ACCENT, buf);
    lv_obj_align(t, LV_ALIGN_CENTER, 0, -150);
    lv_obj_t *h = ui_label(scr, &g_font_text, C_DIM, "离吧唧一手远，正常说话的音量");
    lv_obj_align(h, LV_ALIGN_CENTER, 0, -112);
    s_enroll_line = ui_label(scr, &g_font_text, C_TEXT, "");
    lv_obj_set_width(s_enroll_line, 340);
    lv_label_set_long_mode(s_enroll_line, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(s_enroll_line, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_enroll_line, LV_ALIGN_CENTER, 0, -40);
    s_enroll_lbl = ui_label(scr, &g_font_text, C_GREEN, "");
    lv_obj_set_width(s_enroll_lbl, 320);
    lv_label_set_long_mode(s_enroll_lbl, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(s_enroll_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_enroll_lbl, LV_ALIGN_CENTER, 0, 80);
    show_enroll_line();
    ui_show(scr, enroll_back, enroll_talk);
}

static void on_enroll(lv_event_t *e) { open_enroll((voice_who_t)(intptr_t)lv_event_get_user_data(e)); }

static void on_new_chat(lv_event_t *e)
{
    chat_reset();   /* 小鲸 forgets this conversation; the record stays */
    lv_label_set_text(lv_obj_get_child(lv_event_get_target(e), 0), "已开始新对话");
}

static void on_vp_enable(lv_event_t *e)
{
    voices_set_enabled(lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED));
}

static void on_vp_clear(lv_event_t *e)
{
    voices_reset_all();
    for (int i = 0; i < VOICE_COUNT; i++) {
        if (!s_vp_btns[i]) continue;
        lv_obj_set_style_bg_color(s_vp_btns[i], lv_color_hex(0x2f5bea), 0);
        char label[32];
        snprintf(label, sizeof(label), "录入%s", voices_name(i));
        lv_label_set_text(lv_obj_get_child(s_vp_btns[i], 0), label);
    }
    if (s_vp_clear_lbl) lv_label_set_text(s_vp_clear_lbl, "已清空全部声纹");
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
    snprintf(buf, sizeof(buf), "亮度 %d%%", g_prefs.brightness);
    s_bright_lbl = row_title(buf);
    slider(g_prefs.brightness, 10, on_brightness);
    if (g_prefs.volume > 0) s_set_vol_prev = g_prefs.volume;
    s_vol_lbl = row_title("");
    s_vol_slider = slider(g_prefs.volume, 0, on_volume);
    lv_obj_t *vrow = lv_obj_create(s_cur);
    lv_obj_remove_style_all(vrow);
    lv_obj_set_size(vrow, 240, 42);
    lv_obj_set_flex_flow(vrow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(vrow, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(vrow, 10, 0);
    lv_obj_set_style_margin_ver(vrow, 4, 0);
    lv_obj_remove_flag(vrow, LV_OBJ_FLAG_SCROLLABLE);
    ui_button(vrow, LV_SYMBOL_MINUS, &g_font_text, 66, 40, C_PANEL, on_set_vol_minus, NULL);
    s_vol_mute_btn = ui_button(vrow, NULL, &g_font_text, 88, 40, C_PANEL, on_set_vol_mute, NULL);
    s_vol_mute_lbl = ui_label(s_vol_mute_btn, &g_font_text, 0xffffff, "");
    lv_obj_center(s_vol_mute_lbl);
    ui_button(vrow, LV_SYMBOL_PLUS, &g_font_text, 66, 40, C_PANEL, on_set_vol_plus, NULL);
    ui_settings_sync_volume();

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
    row_title("识别说话人（关闭更适合独享）");
    lv_obj_t *sw_vp = lv_switch_create(s_cur);
    lv_obj_set_style_bg_color(sw_vp, lv_color_hex(C_GREEN), LV_PART_INDICATOR | LV_STATE_CHECKED);
    if (voices_enabled()) lv_obj_add_state(sw_vp, LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw_vp, on_vp_enable, LV_EVENT_VALUE_CHANGED, NULL);
    for (int i = 0; i < VOICE_COUNT; i++) {
        int n = voices_enrolled_n(i);
        bool done = n >= VOICE_ENROLL_SENTENCES;
        if (done) snprintf(buf, sizeof(buf), "%s（已录·点重录）", voices_name(i));
        else if (voices_enrolled_hz(i)) snprintf(buf, sizeof(buf), "%s（需重录）", voices_name(i));
        else snprintf(buf, sizeof(buf), "录入%s", voices_name(i));
        s_vp_btns[i] = pill_ud(buf, done ? C_PANEL : 0x2f5bea, on_enroll, (void *)(intptr_t)i);
    }
    pill("清空全部声纹", C_PANEL, on_vp_clear, &s_vp_clear_lbl);

    card("省电");
    row_title("自动熄屏（点一下换）");
    pill(PREFS_SLEEP_NAMES[g_prefs.sleep], C_PANEL, on_sleep, &s_sleep_btn_lbl);

    card("Wi-Fi");
    char stbuf[160], pass[65];
    net_status_text(stbuf, sizeof(stbuf));
    s_settings_net_lbl = ui_label(s_cur, &g_font_text,
                                  net_up() ? C_GREEN : (net_state() == NET_STATE_FAILED ? C_RED : C_TEXT), stbuf);
    lv_obj_set_width(s_settings_net_lbl, COL_W - 32);
    lv_label_set_long_mode(s_settings_net_lbl, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(s_settings_net_lbl, LV_TEXT_ALIGN_CENTER, 0);

    pill_ud(LV_SYMBOL_WIFI "  扫描附近 Wi-Fi", 0x2f5bea, on_scan_wifi, NULL);

    bool any = false;
    for (int i = 0; i < WIFI_SLOTS; i++) {
        char sname[33];
        if (!settings_get_wifi(i, sname, sizeof(sname), pass, sizeof(pass))) continue;
        memset(pass, 0, sizeof(pass));
        if (!any) row_title("记住的网络（点名字连，点×删）");
        any = true;
        bool connected = net_up() && i == net_slot();
        lv_obj_t *row = lv_obj_create(s_cur);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, 256, 46);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_margin_ver(row, 3, 0);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *cb = ui_button(row, NULL, &g_font_text, 200, 44, connected ? 0x1f5c3a : C_PANEL,
                                 on_connect_saved, (void *)(intptr_t)i);
        lv_obj_set_style_pad_hor(cb, 12, 0);
        lv_obj_t *cl = ui_label(cb, &g_font_text, 0xffffff, sname);
        lv_obj_set_width(cl, 176);
        lv_label_set_long_mode(cl, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_center(cl);

        lv_obj_t *db = ui_button(row, NULL, &g_font_text, 48, 44, C_PANEL, on_forget_wifi, (void *)(intptr_t)i);
        lv_obj_t *dl = ui_label(db, &g_font_text, 0xffffff, LV_SYMBOL_CLOSE);
        lv_obj_center(dl);
    }
    pill_ud("扫码传密码（备选）", C_PANEL, on_add_wifi, NULL);

    card(NULL);
    pill("重启", C_PANEL, on_restart, NULL);
    pill("关机", C_RED, on_power_off, NULL);

    lv_obj_add_event_cb(scr, on_settings_delete, LV_EVENT_DELETE, NULL);
    s_settings_scr = scr;
    static lv_timer_t *net_tmr;
    if (!net_tmr) net_tmr = lv_timer_create(settings_net_timer, 1000, NULL);

    ui_show(scr, ui_home, NULL);
    ui_swipe_edge_only(true);
}

void ui_wifi_add(void) { on_add_wifi(NULL); }
void ui_wifi_scan(void) { open_wifi_scan(); }
void ui_wifi_pass_test(const char *ssid) { open_wifi_pass(ssid && *ssid ? ssid : "我的手机热点", -1); }
