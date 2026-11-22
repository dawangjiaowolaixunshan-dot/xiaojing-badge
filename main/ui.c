#include "ui.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "bsp/display.h"
#include "bsp/esp-bsp.h"
#include "bsp/touch.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "esp_rom_sys.h"
#include <stdlib.h>
#include "esp_lv_adapter.h"
#include "esp_pm.h"
#include "esp_timer.h"
#include "asr.h"
#include "chat.h"
#include "mic.h"
#include "net.h"
#include "pet.h"
#include "pmu.h"
#include "power.h"
#include "prefs.h"
#include "tts.h"
#include "wifi_prov.h"

static const char *TAG = "ui";
LV_FONT_DECLARE(font_text_22)
LV_FONT_DECLARE(font_poem_32)
LV_FONT_DECLARE(font_clock_60)
lv_font_t g_font_text, g_font_poem, g_font_clock;

static void (*s_back)(void);
static void (*s_talk)(bool);
static bool s_swipe_edge_only;
static void swipe_reset(void);

bool ui_lock(void) { return esp_lv_adapter_lock(-1) == ESP_OK; }
void ui_unlock(void) { esp_lv_adapter_unlock(); }

lv_obj_t *ui_screen(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    return scr;
}

void ui_show(lv_obj_t *scr, void (*back)(void), void (*talk)(bool pressed))
{
    swipe_reset();
    s_swipe_edge_only = false;
    s_back = back;
    s_talk = talk;
    /* Instant load: in single-buffer DIRECT mode, FADE_IN blends over a temp layer and can flash. */
    lv_screen_load_anim(scr, LV_SCREEN_LOAD_ANIM_NONE, 0, 0, true);
}

lv_obj_t *ui_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, text ? text : "");
    return l;
}

lv_obj_t *ui_button(lv_obj_t *parent, const char *text, const lv_font_t *font, int w, int h, uint32_t bg,
                    lv_event_cb_t cb, void *user)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_radius(b, h / 2, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(bg), 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x555555), LV_STATE_PRESSED);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, user);
    if (text) {
        lv_obj_t *l = ui_label(b, font, 0xffffff, text);
        lv_obj_center(l);
    }
    return b;
}

/* ---- Global volume overlay & quick corner button ---- */

static lv_obj_t *s_vol_btn, *s_vol_btn_lbl;
static lv_obj_t *s_vol_mask, *s_vol_lbl, *s_vol_slider, *s_vol_mute_lbl, *s_vol_mute_btn;
static lv_timer_t *s_vol_auto_tmr;
static int s_vol_before_mute = 75;
static bool s_vol_dirty;

static const char *vol_icon(int pct)
{
    return pct <= 0 ? LV_SYMBOL_MUTE : pct < 50 ? LV_SYMBOL_VOLUME_MID : LV_SYMBOL_VOLUME_MAX;
}

static void refresh_vol_widgets(void)
{
    int v = g_prefs.volume;
    if (s_vol_btn_lbl) {
        lv_label_set_text(s_vol_btn_lbl, vol_icon(v));
        lv_obj_set_style_text_color(s_vol_btn_lbl, lv_color_hex(v <= 0 ? C_RED : 0xffffff), 0);
    }
    if (s_vol_lbl) {
        char buf[40];
        if (v <= 0) snprintf(buf, sizeof(buf), LV_SYMBOL_MUTE "  已静音");
        else snprintf(buf, sizeof(buf), "%s  音量 %d%%", vol_icon(v), v);
        lv_label_set_text(s_vol_lbl, buf);
        lv_obj_set_style_text_color(s_vol_lbl, lv_color_hex(v <= 0 ? C_RED : C_TEXT), 0);
    }
    if (s_vol_slider) {
        if (lv_slider_get_value(s_vol_slider) != v) lv_slider_set_value(s_vol_slider, v, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(s_vol_slider, lv_color_hex(v <= 0 ? 0x555c6e : C_ACCENT), LV_PART_INDICATOR);
    }
    if (s_vol_mute_lbl && s_vol_mute_btn) {
        lv_label_set_text(s_vol_mute_lbl, v <= 0 ? "恢复" : "静音");
        lv_obj_set_style_bg_color(s_vol_mute_btn, lv_color_hex(v <= 0 ? 0x26734d : C_PANEL), 0);
    }
}

void ui_volume_apply(int pct, bool save)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    if (pct > 0) s_vol_before_mute = pct;
    g_prefs.volume = (uint8_t)pct;
    tts_set_volume(pct);
    refresh_vol_widgets();
    ui_settings_sync_volume();
    if (save) {
        prefs_save();
        s_vol_dirty = false;
    } else {
        s_vol_dirty = true;
    }
}

static void on_vol_auto_close(lv_timer_t *t) { ui_volume_close(); }

static void vol_reset_timer(void)
{
    if (s_vol_auto_tmr) lv_timer_reset(s_vol_auto_tmr);
    else s_vol_auto_tmr = lv_timer_create(on_vol_auto_close, 3500, NULL);
}

static void on_vol_mask_delete(lv_event_t *e)
{
    if (lv_event_get_target(e) != s_vol_mask) return;
    if (s_vol_auto_tmr) {
        lv_timer_delete(s_vol_auto_tmr);
        s_vol_auto_tmr = NULL;
    }
    if (s_vol_dirty) {
        prefs_save();
        s_vol_dirty = false;
    }
    s_vol_mask = s_vol_lbl = s_vol_slider = s_vol_mute_lbl = s_vol_mute_btn = NULL;
}

static void on_vol_mask_tap(lv_event_t *e)
{
    if (lv_event_get_target(e) == s_vol_mask) ui_volume_close();
}

static void on_vol_slider(lv_event_t *e)
{
    int val = lv_slider_get_value(lv_event_get_target(e));
    bool released = lv_event_get_code(e) == LV_EVENT_RELEASED;
    ui_volume_apply(val, released);
    vol_reset_timer();
}

static void on_vol_minus(lv_event_t *e)
{
    int v = (int)g_prefs.volume - 10;
    if (v < 0) v = 0;
    ui_volume_apply(v, true);
    vol_reset_timer();
}

static void on_vol_plus(lv_event_t *e)
{
    int v = (int)g_prefs.volume + 10;
    if (v > 100) v = 100;
    ui_volume_apply(v, true);
    vol_reset_timer();
}

static void on_vol_mute(lv_event_t *e)
{
    if (g_prefs.volume > 0) {
        s_vol_before_mute = g_prefs.volume;
        ui_volume_apply(0, true);
    } else {
        ui_volume_apply(s_vol_before_mute > 0 ? s_vol_before_mute : 75, true);
    }
    vol_reset_timer();
}

bool ui_volume_is_open(void) { return s_vol_mask != NULL; }

void ui_volume_close(void)
{
    if (!s_vol_mask) return;
    lv_obj_t *m = s_vol_mask;
    lv_obj_delete(m);
}

void ui_volume_popup(void)
{
    lv_obj_t *scr = lv_screen_active();
    if (!scr) return;
    if (s_vol_mask && lv_obj_get_screen(s_vol_mask) == scr) {
        lv_obj_move_foreground(s_vol_mask);
        refresh_vol_widgets();
        vol_reset_timer();
        return;
    }
    if (s_vol_mask) ui_volume_close();
    if (g_prefs.volume > 0) s_vol_before_mute = g_prefs.volume;

    s_vol_mask = lv_obj_create(scr);
    lv_obj_remove_style_all(s_vol_mask);
    lv_obj_set_size(s_vol_mask, LV_PCT(100), LV_PCT(100));
    lv_obj_center(s_vol_mask);
    lv_obj_set_style_bg_color(s_vol_mask, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_vol_mask, LV_OPA_60, 0);
    lv_obj_add_flag(s_vol_mask, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_vol_mask, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_vol_mask, on_vol_mask_tap, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(s_vol_mask, on_vol_mask_delete, LV_EVENT_DELETE, NULL);

    lv_obj_t *card = lv_obj_create(s_vol_mask);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, 324, 218);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x121826), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0x2b3954), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_radius(card, 26, 0);
    lv_obj_set_style_pad_top(card, 22, 0);
    lv_obj_set_style_pad_bottom(card, 20, 0);
    lv_obj_set_style_pad_hor(card, 20, 0);
    lv_obj_set_style_pad_row(card, 18, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    s_vol_lbl = ui_label(card, &g_font_text, C_TEXT, "");

    s_vol_slider = lv_slider_create(card);
    lv_obj_set_size(s_vol_slider, 268, 26);
    lv_slider_set_range(s_vol_slider, 0, 100);
    lv_obj_set_style_radius(s_vol_slider, 13, LV_PART_MAIN);
    lv_obj_set_style_radius(s_vol_slider, 13, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_vol_slider, lv_color_hex(0x252e42), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_vol_slider, lv_color_hex(0xffffff), LV_PART_KNOB);
    lv_obj_set_style_pad_all(s_vol_slider, 6, LV_PART_KNOB);
    lv_obj_set_ext_click_area(s_vol_slider, 14);
    lv_obj_add_event_cb(s_vol_slider, on_vol_slider, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s_vol_slider, on_vol_slider, LV_EVENT_RELEASED, NULL);

    lv_obj_t *row = lv_obj_create(card);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, 270, 48);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 10, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    ui_button(row, LV_SYMBOL_MINUS, &g_font_text, 76, 46, C_PANEL, on_vol_minus, NULL);
    s_vol_mute_btn = ui_button(row, NULL, &g_font_text, 98, 46, C_PANEL, on_vol_mute, NULL);
    s_vol_mute_lbl = ui_label(s_vol_mute_btn, &g_font_text, 0xffffff, "");
    lv_obj_center(s_vol_mute_lbl);
    ui_button(row, LV_SYMBOL_PLUS, &g_font_text, 76, 46, C_PANEL, on_vol_plus, NULL);

    refresh_vol_widgets();
    vol_reset_timer();
}

static void on_vol_btn_tap(lv_event_t *e) { ui_volume_popup(); }

static void on_vol_btn_delete(lv_event_t *e)
{
    if (lv_event_get_target(e) != s_vol_btn) return;
    s_vol_btn = s_vol_btn_lbl = NULL;
}

void ui_volume_button(lv_obj_t *scr)
{
    lv_obj_t *b = ui_button(scr, NULL, &g_font_text, 50, 50, C_PANEL, NULL, NULL);
    lv_obj_align(b, LV_ALIGN_CENTER, 186, -96);
    lv_obj_set_ext_click_area(b, 12);
    lv_obj_add_event_cb(b, on_vol_btn_tap, LV_EVENT_SHORT_CLICKED, NULL);
    lv_obj_add_event_cb(b, on_vol_btn_delete, LV_EVENT_DELETE, NULL);
    lv_obj_t *l = ui_label(b, &g_font_text, g_prefs.volume <= 0 ? C_RED : 0xffffff, vol_icon(g_prefs.volume));
    lv_obj_center(l);
    s_vol_btn = b;
    s_vol_btn_lbl = l;
}

/* ---- Screen off when idle ---- */

static bool s_asleep, s_dimmed, s_net_sleeping;
static uint32_t s_asleep_at;   /* lv_tick when the screen went dark */
#define NET_SLEEP_DELAY_MS 120000   /* keep hotspot alive 2 min after screen off */
static bool s_frozen;          /* the whale held still: nobody's looking */
#define FREEZE_MS 20000
static esp_lcd_panel_handle_t s_panel;   /* set by display_start() */
static esp_lcd_panel_io_handle_t s_io;   /* set by display_start() */
static lv_obj_t *s_wake_cover;   /* catches the touch that wakes, so it presses nothing */

static void wire_idle(int held);

static void panel_sleep_cmd(bool sleep)
{
    if (!s_io) return;
    wire_idle(0);
    /* Plain CO5300 SLPIN (0x10) / SLPOUT (0x11), same as Waveshare 1.75C reference. */
    esp_lcd_panel_io_tx_param(s_io, (0x02 << 24) | ((sleep ? 0x10 : 0x11) << 8), NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(120));
}

static void panel_set_brightness(int pct)
{
    wire_idle(0);
    bsp_display_brightness_set(pct);
}

void ui_set_brightness(int pct)
{
    if (!s_asleep) panel_set_brightness(pct);
}

static void wake(void)
{
    lv_display_trigger_activity(NULL);
    if (s_wake_cover) {
        lv_obj_delete(s_wake_cover);
        s_wake_cover = NULL;
    }
    bool was_asleep = s_asleep;
    bool was_dimmed = s_dimmed;
    s_asleep = s_dimmed = false;
    if (was_asleep) {
        panel_set_brightness(0);
        panel_sleep_cmd(false);
        /* Redraw fresh frame while brightness is still 0 so wake never flashes. */
        lv_obj_invalidate(lv_screen_active());
        lv_refr_now(NULL);
        if (s_net_sleeping) {
            s_net_sleeping = false;
            net_suspend(false);
        }
    }
    if (was_asleep || was_dimmed) panel_set_brightness(g_prefs.brightness);
    if (was_asleep || s_frozen) pet_set_paused(false);
    s_frozen = false;
}

static void on_wake_cover(lv_event_t *e) { wake(); }

/*
 * Frequency scaling lets the CPU idle at 80 MHz, and LVGL then draws a swipe
 * at a third of the speed. Hold it at full speed while the screen is touched
 * or anything is animating, and let it drop again a moment after.
 */
static esp_pm_lock_handle_t s_fast_lock;
static bool s_fast;

static void fast_timer(lv_timer_t *t)
{
    bool want = !s_asleep && (lv_display_get_inactive_time(NULL) < 1500 || lv_anim_count_running() > 0);
    if (want == s_fast || !s_fast_lock) return;
    s_fast = want;
    if (want) esp_pm_lock_acquire(s_fast_lock);
    else esp_pm_lock_release(s_fast_lock);
}

/* Frame times, for >scrolltest: from the start of a refresh to its last flush. */
static int64_t s_refr_at;
static int s_frames, s_frame_max_us;
static int64_t s_frame_sum_us;
static void on_refr(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_REFR_START) {
        s_refr_at = esp_timer_get_time();
    } else if (s_refr_at) {
        int us = (int)(esp_timer_get_time() - s_refr_at);
        s_refr_at = 0;
        if (lv_display_get_refr_timer(NULL) && us > 200) {   /* a refresh that drew something */
            s_frames++;
            s_frame_sum_us += us;
            if (us > s_frame_max_us) s_frame_max_us = us;
        }
    }
}

void ui_frames_reset(void) { s_frames = 0; s_frame_sum_us = 0; s_frame_max_us = 0; }
void ui_frames_get(int *frames, int *avg_us, int *max_us)
{
    *frames = s_frames;
    *avg_us = s_frames ? (int)(s_frame_sum_us / s_frames) : 0;
    *max_us = s_frame_max_us;
}

static void sleep_timer(lv_timer_t *t)
{
    /* Talking, thinking, reading aloud, reciting, or setting up Wi-Fi counts as use. */
    if (chat_busy() || tts_active() || ui_chat_listening() || mic_active() || asr_active() || prov_active() ||
        ui_wifi_busy()) {
        lv_display_trigger_activity(NULL);
    }
    int limit = PREFS_SLEEP_S[g_prefs.sleep] * 1000;
    uint32_t idle = lv_display_get_inactive_time(NULL);
    /* Nobody touching it for a while: hold the whale still (fewer redraws). */
    if (!s_frozen && !s_asleep && idle > FREEZE_MS) {
        s_frozen = true;
        pet_set_paused(true);
    } else if (s_frozen && idle < 1000) {
        s_frozen = false;
        pet_set_paused(false);
    }
    if (!limit || idle < 1000) {
        if ((s_asleep || s_dimmed) && idle < 1000) wake();
        return;
    }
    if (!s_asleep && idle >= (uint32_t)limit) {
        ui_volume_close();
        swipe_reset();
        s_asleep = true;
        panel_set_brightness(0);
        panel_sleep_cmd(true);
        pet_set_paused(true);                                     /* and no frames drawn for nobody */
        s_asleep_at = lv_tick_get();
        s_wake_cover = lv_obj_create(lv_layer_top());
        lv_obj_remove_style_all(s_wake_cover);
        lv_obj_set_size(s_wake_cover, LV_PCT(100), LV_PCT(100));
        lv_obj_add_flag(s_wake_cover, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(s_wake_cover, on_wake_cover, LV_EVENT_PRESSED, NULL);
    } else if (s_asleep) {
        static uint32_t s_usb_poll_at;
        static bool s_usb_plugged;
        if (!s_usb_poll_at || lv_tick_elaps(s_usb_poll_at) >= 2000) {
            int pct;
            bool chg, usb;
            if (pmu_read(&pct, &chg, &usb)) s_usb_plugged = usb;
            s_usb_poll_at = lv_tick_get();
        }
        if (s_usb_plugged) {
            /* Plugged into USB: keep Wi-Fi alive while screen sleeps; re-arm timer on unplug. */
            s_asleep_at = lv_tick_get();
            if (s_net_sleeping) {
                s_net_sleeping = false;
                net_suspend(false);
            }
        } else if (!s_net_sleeping && lv_tick_elaps(s_asleep_at) >= NET_SLEEP_DELAY_MS) {
            s_net_sleeping = true;
            net_suspend(true);                                    /* radio off after 2 min asleep on battery */
        }
    } else if (!s_asleep && !s_dimmed && idle >= (uint32_t)limit - 8000) {
        s_dimmed = true;   /* a warning: it goes dark in a few seconds */
        panel_set_brightness(g_prefs.brightness / 4 + 3);
    }
}

/* A key on a dark screen only wakes it. */
static bool woke_by_key(void)
{
    bool was = s_asleep;
    wake();
    return was;
}

static lv_timer_t *s_boot_long_tmr;
static bool s_boot_long, s_boot_ignore_up;

static void on_boot_long(lv_timer_t *t)
{
    if (s_boot_long_tmr) {
        lv_timer_delete(s_boot_long_tmr);
        s_boot_long_tmr = NULL;
    }
    s_boot_long = true;
    ui_volume_popup();
}

void ui_boot_button(bool pressed)
{
    if (!ui_lock()) return;
    if (pressed) {
        if (woke_by_key()) {
            s_boot_ignore_up = true;
            ui_unlock();
            return;
        }
        s_boot_long = false;
        s_boot_ignore_up = false;
        if (s_boot_long_tmr) lv_timer_reset(s_boot_long_tmr);
        else s_boot_long_tmr = lv_timer_create(on_boot_long, 420, NULL);
    } else {
        if (s_boot_long_tmr) {
            lv_timer_delete(s_boot_long_tmr);
            s_boot_long_tmr = NULL;
        }
        if (s_boot_ignore_up || s_boot_long) {
            s_boot_ignore_up = false;
            s_boot_long = false;
            ui_unlock();
            return;
        }
        if (s_vol_mask) ui_volume_close();
        else if (s_back) s_back();
        else ui_volume_popup();
    }
    ui_unlock();
}

void ui_talk_button(bool pressed)
{
    if (!ui_lock()) return;
    if (pressed && woke_by_key()) {
        ui_unlock();
        return;
    }
    if (s_talk) {
        s_talk(pressed);
    } else if (pressed && !s_back) {
        /* Walkie-talkie shortcut from home: jump to chat and start recording at once. */
        ui_chat();
        if (s_talk) s_talk(true);
    }
    ui_unlock();
}

void ui_settings_changed(void)
{
    /* Screens read the keys when they act; nothing cached to refresh. */
}

/* ---- Android-style right-swipe back with floating edge indicator ---- */

static lv_indev_t *s_tp_indev;
static lv_obj_t *s_sw_pill, *s_sw_pill_lbl;
static lv_timer_t *s_sw_tmr;
static bool s_sw_track, s_sw_active, s_sw_block_click;
static int s_sw_x0, s_sw_y0, s_sw_dx;

static int arc_left_x(int y)
{
    float dy = (float)(y - 233);
    float r2 = 233.0f * 233.0f - dy * dy;
    return r2 > 0.0f ? (int)(233.0f - sqrtf(r2)) : 233;
}

static void on_sw_pill_delete(lv_event_t *e)
{
    if (lv_event_get_target(e) != s_sw_pill) return;
    s_sw_pill = s_sw_pill_lbl = NULL;
}

static void swipe_pill_clear(void)
{
    if (!s_sw_pill) return;
    lv_obj_t *p = s_sw_pill;
    lv_obj_delete(p);
}

static void swipe_pill_update(int cy, int dx)
{
    lv_obj_t *scr = lv_screen_active();
    if (!scr) return;
    if (s_sw_pill && lv_obj_get_screen(s_sw_pill) != scr) swipe_pill_clear();
    if (!s_sw_pill) {
        s_sw_pill = lv_obj_create(scr);
        lv_obj_remove_style_all(s_sw_pill);
        lv_obj_set_size(s_sw_pill, 46, 46);
        lv_obj_set_style_radius(s_sw_pill, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(s_sw_pill, LV_OPA_90, 0);
        lv_obj_set_style_border_width(s_sw_pill, 1, 0);
        lv_obj_remove_flag(s_sw_pill, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(s_sw_pill, on_sw_pill_delete, LV_EVENT_DELETE, NULL);
        s_sw_pill_lbl = ui_label(s_sw_pill, &g_font_text, 0xffffff, LV_SYMBOL_LEFT);
        lv_obj_center(s_sw_pill_lbl);
    }
    if (cy < 120) cy = 120;
    if (cy > 346) cy = 346;
    if (dx < 0) dx = 0;
    int base = arc_left_x(cy);
    int pull = (int)(38.0f * (1.0f - expf(-(float)dx / 85.0f)));
    lv_obj_set_pos(s_sw_pill, base + 4 + pull, cy - 23);
    bool ready = dx >= 74;
    lv_obj_set_style_bg_color(s_sw_pill, lv_color_hex(ready ? C_ACCENT : 0x222938), 0);
    lv_obj_set_style_border_color(s_sw_pill, lv_color_hex(ready ? 0xffb478 : 0x4d5b78), 0);
    lv_obj_move_foreground(s_sw_pill);
}

static void swipe_reset(void)
{
    s_sw_track = false;
    s_sw_active = false;
    s_sw_dx = 0;
    if (s_sw_tmr) {
        lv_timer_delete(s_sw_tmr);
        s_sw_tmr = NULL;
    }
    swipe_pill_clear();
}

static void swipe_async_back(void *arg)
{
    void (*fn)(void) = (void (*)(void))arg;
    if (fn && fn == s_back) fn();
}

static void swipe_track_timer(lv_timer_t *t)
{
    if (!s_sw_track || !s_back || s_asleep || s_vol_mask || !s_tp_indev ||
        lv_indev_get_state(s_tp_indev) != LV_INDEV_STATE_PRESSED) {
        swipe_reset();
        return;
    }
    lv_point_t pt;
    lv_indev_get_point(s_tp_indev, &pt);
    int dx = pt.x - s_sw_x0;
    int dy = pt.y - s_sw_y0;
    int ady = dy < 0 ? -dy : dy;
    if (!s_sw_active) {
        if (dx < -16 || (ady > 20 && ady * 10 > dx * 7) ||
            (lv_indev_get_scroll_obj(s_tp_indev) != NULL && ady > 16)) {
            swipe_reset();
            return;
        }
        if (dx >= 20 && dx * 10 >= ady * 14) {
            s_sw_active = true;
            s_sw_block_click = true;
            s_sw_dx = dx;
            swipe_pill_update(s_sw_y0, s_sw_dx);
        }
    } else {
        s_sw_dx = dx > 0 ? dx : 0;
        swipe_pill_update(s_sw_y0, s_sw_dx);
    }
}

static void on_tp_event(lv_event_t *e)
{
    if (!s_tp_indev) return;
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED) {
        s_sw_block_click = false;
        if (!s_back || s_asleep || s_vol_mask) return;
        lv_obj_t *target = (lv_obj_t *)lv_event_get_param(e);
        if (target && lv_obj_check_type(target, &lv_slider_class)) return;
        lv_point_t pt;
        lv_indev_get_point(s_tp_indev, &pt);
        if (s_swipe_edge_only && pt.x > arc_left_x(pt.y) + 100) return;
        s_sw_x0 = pt.x;
        s_sw_y0 = pt.y;
        s_sw_dx = 0;
        s_sw_track = true;
        s_sw_active = false;
        if (s_sw_tmr) lv_timer_reset(s_sw_tmr);
        else s_sw_tmr = lv_timer_create(swipe_track_timer, 16, NULL);
    } else if (code == LV_EVENT_RELEASED) {
        if (s_sw_active) {
            lv_point_t pt;
            lv_indev_get_point(s_tp_indev, &pt);
            int dx = pt.x - s_sw_x0;
            bool go = dx >= 74;
            void (*fn)(void) = s_back;
            lv_indev_stop_processing(s_tp_indev);
            swipe_reset();
            if (go && fn) lv_async_call(swipe_async_back, (void *)fn);
        } else if (s_sw_track) {
            swipe_reset();
        }
    } else if (code == LV_EVENT_SHORT_CLICKED || code == LV_EVENT_CLICKED) {
        if (s_sw_block_click) lv_indev_stop_processing(s_tp_indev);
    }
}

void ui_swipe_edge_only(bool edge_only) { s_swipe_edge_only = edge_only; }

void ui_swipe_preview(int dx)
{
    if (dx <= 0) swipe_pill_clear();
    else swipe_pill_update(233, dx);
}

static void set_metrics(lv_font_t *dst, const lv_font_t *src, int line_h, int base)
{
    *dst = *src;
    dst->line_height = line_h;   /* the converter's max is set by a few tall symbols */
    dst->base_line = base;
    dst->fallback = &lv_font_montserrat_20;   /* LV_SYMBOL_* icons */
}

/* The CO5300 takes update windows on even rows and columns only. */
static void round_area(lv_event_t *e)
{
    lv_area_t *a = lv_event_get_param(e);
    a->x1 &= ~1;
    a->y1 &= ~1;
    a->x2 |= 1;
    a->y2 |= 1;
}

/*
 * Tear-free updates. The CO5300's TE output is wired to GPIO 13 (not in the
 * BSP; found with >teprobe): a pulse each time the panel starts a scan, ~57
 * times a second. LVGL draws the changed areas into a full-screen buffer in
 * PSRAM (direct mode); when a frame is complete, each area goes over the bus
 * either before the scan reaches it or just after the scan has passed it, so
 * the panel never shows half an update. Areas were sent strip by strip as
 * they were drawn before, and a swipe showed as bands sweeping down.
 */
#define TE_GPIO 13
#define BOUNCE_ROWS 24             /* rows per transfer, through internal DMA RAM */
#define LCD_W BSP_LCD_H_RES
#define LCD_H BSP_LCD_V_RES
#define MAX_AREAS 32
static uint16_t *s_fb;
static uint8_t *s_bounce[2];
static SemaphoreHandle_t s_te_sem, s_buf_free;
static volatile int64_t s_te_at;
static volatile int32_t s_te_period = 17500;   /* us, measured */
static esp_lcd_panel_handle_t s_te_panel;
static lv_area_t s_areas[MAX_AREAS];
static int s_nareas;

static void IRAM_ATTR on_te(void *arg)
{
    int64_t now = esp_timer_get_time();
    int64_t d = now - s_te_at;
    if (d > 8000 && d < 40000) s_te_period = (s_te_period * 7 + (int32_t)d) / 8;
    s_te_at = now;
    BaseType_t woke = pdFALSE;
    xSemaphoreGiveFromISR(s_te_sem, &woke);
    if (woke) portYIELD_FROM_ISR();
}

static bool IRAM_ATTR on_trans_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *e, void *ctx)
{
    BaseType_t woke = pdFALSE;
    xSemaphoreGiveFromISR(s_buf_free, &woke);
    return woke == pdTRUE;
}

static void wire_idle(int held)
{
    if (!s_buf_free) return;
    for (int i = held; i < 2; i++) xSemaphoreTake(s_buf_free, portMAX_DELAY);
    for (int i = held; i < 2; i++) xSemaphoreGive(s_buf_free);
}

static void send_area(const lv_area_t *a)
{
    int w = a->x2 - a->x1 + 1;
    static int k;
    for (int y = a->y1; y <= a->y2; y += BOUNCE_ROWS) {
        int rows = a->y2 + 1 - y < BOUNCE_ROWS ? a->y2 + 1 - y : BOUNCE_ROWS;
        xSemaphoreTake(s_buf_free, portMAX_DELAY);
        uint16_t *dst = (uint16_t *)s_bounce[k];
        for (int r = 0; r < rows; r++) {
            const uint16_t *src = s_fb + (y + r) * LCD_W + a->x1;
            for (int i = 0; i < w; i++) dst[i] = (uint16_t)(src[i] << 8 | src[i] >> 8);   /* the panel takes RGB565 big-endian */
            dst += w;
        }
        /* Wait for any previous QSPI piece to finish before acquiring SPI bus lock
         * (avoids ESP-IDF 6.0 spi_bus_lock handoff race). */
        wire_idle(1);
        if (esp_lcd_panel_draw_bitmap(s_te_panel, a->x1, y, a->x2 + 1, y + rows, s_bounce[k]) != ESP_OK) {
            xSemaphoreGive(s_buf_free);
        }
        k ^= 1;
    }
}

static int cmp_area(const void *a, const void *b) { return ((const lv_area_t *)a)->y1 - ((const lv_area_t *)b)->y1; }

static void wait_until(int64_t t)
{
    int64_t now;
    while ((now = esp_timer_get_time()) < t) {
        if (t - now > 2000) vTaskDelay(1);
        else esp_rom_delay_us((uint32_t)(t - now));
    }
}

static int64_t read_te_at(void)
{
    int64_t t;
    do { t = s_te_at; } while (t != s_te_at);   /* atomic 64-bit read on 32-bit Xtensa */
    return t;
}

static void te_flush(lv_display_t *disp, const lv_area_t *area, uint8_t *px)
{
    if (s_nareas < MAX_AREAS) {
        s_areas[s_nareas++] = *area;
    } else {   /* too many: grow the last into a box around them */
        lv_area_t *l = &s_areas[MAX_AREAS - 1];
        lv_area_t u = {LV_MIN(l->x1, area->x1), LV_MIN(l->y1, area->y1), LV_MAX(l->x2, area->x2), LV_MAX(l->y2, area->y2)};
        *l = u;
    }
    if (!lv_display_flush_is_last(disp)) {
        lv_display_flush_ready(disp);
        return;
    }
    if (s_asleep) {
        s_nareas = 0;
        lv_display_flush_ready(disp);
        return;
    }
    /* Keep CPU clock steady during QSPI DMA so DFS never switches mid-transfer. */
    if (s_fast_lock) esp_pm_lock_acquire(s_fast_lock);
    qsort(s_areas, s_nareas, sizeof(lv_area_t), cmp_area);
    xSemaphoreTake(s_te_sem, 0);                              /* a stale pulse */
    bool synced = xSemaphoreTake(s_te_sem, pdMS_TO_TICKS(30)) == pdTRUE;   /* none when the panel is off */
    int64_t t0 = read_te_at();
    float row_us = s_te_period * 0.92f / LCD_H;               /* the scan, leaving a little for blanking */
    for (int i = 0; i < s_nareas; i++) {
        const lv_area_t *a = &s_areas[i];
        if (synced) {
            int64_t now = esp_timer_get_time() - t0;
            float send_us = (float)lv_area_get_size(a) / 10.0f + 60.0f * ((a->y2 - a->y1) / BOUNCE_ROWS + 1);   /* 20 MB/s */
            bool ahead = now + send_us < a->y1 * row_us;      /* done before the scan gets there */
            if (!ahead) wait_until(t0 + (int64_t)((a->y2 + 2) * row_us));   /* else right behind it */
        }
        send_area(a);
    }
    wire_idle(0);
    if (s_fast_lock) esp_pm_lock_release(s_fast_lock);
    s_nareas = 0;
    lv_display_flush_ready(disp);
}

static bool te_start(lv_display_t *disp, esp_lcd_panel_handle_t panel, esp_lcd_panel_io_handle_t io)
{
    size_t fb_bytes = LCD_W * LCD_H * 2;
    s_fb = heap_caps_aligned_alloc(64, fb_bytes, MALLOC_CAP_SPIRAM);
    for (int i = 0; i < 2; i++) s_bounce[i] = heap_caps_malloc(LCD_W * BOUNCE_ROWS * 2, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    s_te_sem = xSemaphoreCreateBinary();
    s_buf_free = xSemaphoreCreateCounting(2, 2);
    if (!s_fb || !s_bounce[0] || !s_bounce[1] || !s_te_sem || !s_buf_free) {
        ESP_LOGE(TAG, "tear-free display: no memory");
        return false;
    }
    memset(s_fb, 0, fb_bytes);
    s_te_panel = panel;
    esp_lcd_panel_io_callbacks_t cbs = {.on_color_trans_done = on_trans_done};
    esp_lcd_panel_io_register_event_callbacks(io, &cbs, NULL);
    gpio_config_t te = {.pin_bit_mask = 1ULL << TE_GPIO, .mode = GPIO_MODE_INPUT, .intr_type = GPIO_INTR_POSEDGE};
    gpio_config(&te);
    esp_err_t r = gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
    if (r != ESP_OK && r != ESP_ERR_INVALID_STATE) return false;
    gpio_isr_handler_add(TE_GPIO, on_te, NULL);
    lv_display_set_buffers(disp, s_fb, NULL, fb_bytes, LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(disp, te_flush);
    ESP_LOGI(TAG, "tear-free display: TE on GPIO %d, direct mode", TE_GPIO);
    return true;
}

/*
 * bsp_display_start(), with its draw buffers in internal RAM instead of
 * PSRAM. From PSRAM, every flush borrows a 46 KB internal DMA bounce buffer,
 * which fails once speech, TTS and DeepSeek connections are all up ("Failed
 * to allocate priv TX buffer"): the screen stops redrawing and the network
 * runs short too. Two 24-row buffers (22 KB each), taken once at boot, avoid
 * that.
 */
static lv_display_t *display_start(void)
{
    esp_lv_adapter_config_t adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    if (esp_lv_adapter_init(&adapter_cfg) != ESP_OK) return NULL;
    esp_lcd_panel_handle_t panel;
    esp_lcd_panel_io_handle_t io;
    const bsp_display_config_t panel_cfg = {
        .max_transfer_sz = BSP_LCD_H_RES * BOUNCE_ROWS * 2,
    };
    if (bsp_display_new(&panel_cfg, &panel, &io) != ESP_OK) return NULL;
    s_panel = panel;
    s_io = io;
    /* Mute brightness immediately so uninitialized GRAM and LVGL's default
     * white screen never flash before ui_home() renders its first frame. */
    bsp_display_brightness_set(0);
    esp_lv_adapter_set_default_display_idf_callback_registration_enabled(false);
    esp_lv_adapter_display_config_t disp_cfg = {
        .panel = panel,
        .panel_io = io,
        .profile = {
            .interface = ESP_LV_ADAPTER_PANEL_IF_OTHER,
            .rotation = ESP_LV_ADAPTER_ROTATE_0,
            .hor_res = BSP_LCD_H_RES,
            .ver_res = BSP_LCD_V_RES,
            .buffer_height = 2,   /* unused: te_start() gives LVGL its own buffer */
            .use_psram = false,
            .require_double_buffer = true,
        },
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE,
    };
    lv_display_t *disp = esp_lv_adapter_register_display(&disp_cfg);
    if (!disp) return NULL;
    lv_display_add_event_cb(disp, round_area, LV_EVENT_INVALIDATE_AREA, NULL);
    if (!te_start(disp, panel, io)) return NULL;
    /* Clear panel GRAM to black while brightness is 0. */
    lv_area_t full = {0, 0, LCD_W - 1, LCD_H - 1};
    send_area(&full);
    wire_idle(0);
    /* Paint LVGL's initial screen and bottom layer black before starting LVGL task. */
    lv_obj_set_style_bg_color(lv_layer_bottom(), lv_color_black(), 0);
    lv_obj_set_style_bg_opa(lv_layer_bottom(), LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(lv_screen_active(), lv_color_black(), 0);
    lv_obj_set_style_bg_opa(lv_screen_active(), LV_OPA_COVER, 0);
    lv_display_add_event_cb(disp, on_refr, LV_EVENT_REFR_START, NULL);
    lv_display_add_event_cb(disp, on_refr, LV_EVENT_REFR_READY, NULL);

    const bsp_display_cfg_t touch_cfg = {
        .touch_flags = {.mirror_x = 1, .mirror_y = 1},
    };
    esp_lcd_touch_handle_t tp;
    if (bsp_touch_new(&touch_cfg, &tp) != ESP_OK) return NULL;
    const esp_lv_adapter_touch_config_t tp_cfg = ESP_LV_ADAPTER_TOUCH_DEFAULT_CONFIG(disp, tp);
    s_tp_indev = esp_lv_adapter_register_touch(&tp_cfg);
    if (!s_tp_indev) return NULL;
    lv_indev_add_event_cb(s_tp_indev, on_tp_event, LV_EVENT_ALL, NULL);
    if (esp_lv_adapter_start() != ESP_OK) return NULL;
    return disp;
}

void ui_start(void)
{
    set_metrics(&g_font_text, &font_text_22, 27, 6);
    set_metrics(&g_font_poem, &font_poem_32, 38, 9);
    g_font_clock = font_clock_60;
    lv_display_t *disp = display_start();
    if (!disp) {
        ESP_LOGE(TAG, "display start failed");
        return;
    }
    ui_lock();
    prefs_load();
    ui_home();
    lv_refr_now(NULL);
    prefs_apply();   /* turn brightness up only after the first black/home frame is in GRAM */
    lv_timer_create(sleep_timer, 500, NULL);
    if (esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "ui", &s_fast_lock) == ESP_OK) lv_timer_create(fast_timer, 30, NULL);
    ui_unlock();
}

bool ui_screen_on(void) { return !s_asleep; }

void ui_prepare_deep_sleep(void)
{
    if (s_panel) esp_lcd_panel_disp_sleep(s_panel, true);
    tts_stop();
}
