#include "ui.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "bsp/display.h"
#include "bsp/esp-bsp.h"
#include "bsp/touch.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "chat.h"
#include "net.h"
#include "pet.h"
#include "pmu.h"
#include "power.h"
#include "prefs.h"
#include "tts.h"

static const char *TAG = "ui";
LV_FONT_DECLARE(font_text_22)
LV_FONT_DECLARE(font_poem_32)
LV_FONT_DECLARE(font_clock_60)
lv_font_t g_font_text, g_font_poem, g_font_clock;

static void (*s_back)(void);
static void (*s_talk)(bool);

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
    s_back = back;
    s_talk = talk;
    lv_screen_load_anim(scr, LV_SCREEN_LOAD_ANIM_FADE_IN, 150, 0, true);
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

static void on_back(lv_event_t *e)
{
    if (s_back) s_back();
}

void ui_back_button(lv_obj_t *scr)
{
    lv_obj_t *b = ui_button(scr, LV_SYMBOL_LEFT, &g_font_text, 56, 56, C_PANEL, on_back, NULL);
    lv_obj_align(b, LV_ALIGN_CENTER, -186, -96);
    lv_obj_set_ext_click_area(b, 12);
}

/* ---- Screen off when idle ---- */

static bool s_asleep, s_dimmed;
static uint32_t s_asleep_at;   /* lv_tick when the screen went dark */
static bool s_frozen;          /* the whale held still: nobody's looking */
#define FREEZE_MS 20000
static esp_lcd_panel_handle_t s_panel;   /* set by display_start() */
static lv_obj_t *s_wake_cover;   /* catches the touch that wakes, so it presses nothing */

static void wake(void)
{
    lv_display_trigger_activity(NULL);
    if (s_wake_cover) {
        lv_obj_delete(s_wake_cover);
        s_wake_cover = NULL;
    }
    if (s_asleep && s_panel) esp_lcd_panel_disp_on_off(s_panel, true);
    if (s_asleep || s_dimmed) bsp_display_brightness_set(g_prefs.brightness);
    if (s_asleep) net_suspend(false);
    if (s_asleep || s_frozen) pet_set_paused(false);
    s_frozen = false;
    s_asleep = s_dimmed = false;
}

static void on_wake_cover(lv_event_t *e) { wake(); }

static void sleep_timer(lv_timer_t *t)
{
    /* Talking, thinking or reading aloud counts as use. */
    if (chat_busy() || tts_active() || ui_chat_listening()) lv_display_trigger_activity(NULL);
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
        s_asleep = true;
        bsp_display_brightness_set(0);
        if (s_panel) esp_lcd_panel_disp_on_off(s_panel, false);   /* the panel itself off, not just dark */
        pet_set_paused(true);                                     /* and no frames drawn for nobody */
        net_suspend(true);                                        /* nor a radio kept up for nobody */
        s_asleep_at = lv_tick_get();
        s_wake_cover = lv_obj_create(lv_layer_top());
        lv_obj_remove_style_all(s_wake_cover);
        lv_obj_set_size(s_wake_cover, LV_PCT(100), LV_PCT(100));
        lv_obj_add_flag(s_wake_cover, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(s_wake_cover, on_wake_cover, LV_EVENT_PRESSED, NULL);
    } else if (!s_asleep && !s_dimmed && idle >= (uint32_t)limit - 8000) {
        s_dimmed = true;   /* a warning: it goes dark in a few seconds */
        bsp_display_brightness_set(g_prefs.brightness / 4 + 3);
    }
}

/* A key on a dark screen only wakes it. */
static bool woke_by_key(void)
{
    bool was = s_asleep;
    wake();
    return was;
}

void ui_boot_button(bool pressed)
{
    if (!ui_lock()) return;
    if (woke_by_key()) {
        ui_unlock();
        return;
    }
    if (!pressed && s_back) s_back();
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
        ui_chat();   /* on the home screen, PWR goes to DeepSeek */
    }
    ui_unlock();
}

void ui_settings_changed(void)
{
    /* Screens read the keys when they act; nothing cached to refresh. */
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
 * bsp_display_start(), with its draw buffers in internal RAM instead of
 * PSRAM. From PSRAM, every flush borrows a 46 KB internal DMA bounce buffer,
 * which fails once speech, TTS and DeepSeek connections are all up ("Failed
 * to allocate priv TX buffer"): the screen stops redrawing and the network
 * runs short too. Two 24-row buffers (22 KB each), taken once at boot, avoid
 * that.
 */
#define DRAW_ROWS 24
static lv_display_t *display_start(void)
{
    esp_lv_adapter_config_t adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    if (esp_lv_adapter_init(&adapter_cfg) != ESP_OK) return NULL;
    esp_lcd_panel_handle_t panel;
    esp_lcd_panel_io_handle_t io;
    const bsp_display_config_t panel_cfg = {
        .max_transfer_sz = BSP_LCD_H_RES * DRAW_ROWS * 2,
    };
    if (bsp_display_new(&panel_cfg, &panel, &io) != ESP_OK) return NULL;
    s_panel = panel;
    esp_lv_adapter_display_config_t disp_cfg = {
        .panel = panel,
        .panel_io = io,
        .profile = {
            .interface = ESP_LV_ADAPTER_PANEL_IF_OTHER,
            .rotation = ESP_LV_ADAPTER_ROTATE_0,
            .hor_res = BSP_LCD_H_RES,
            .ver_res = BSP_LCD_V_RES,
            .buffer_height = DRAW_ROWS,
            .use_psram = false,
            .require_double_buffer = true,
        },
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE,
    };
    lv_display_t *disp = esp_lv_adapter_register_display(&disp_cfg);
    if (!disp) return NULL;
    lv_display_add_event_cb(disp, round_area, LV_EVENT_INVALIDATE_AREA, NULL);

    const bsp_display_cfg_t touch_cfg = {
        .touch_flags = {.mirror_x = 1, .mirror_y = 1},
    };
    esp_lcd_touch_handle_t tp;
    if (bsp_touch_new(&touch_cfg, &tp) != ESP_OK) return NULL;
    const esp_lv_adapter_touch_config_t tp_cfg = ESP_LV_ADAPTER_TOUCH_DEFAULT_CONFIG(disp, tp);
    if (!esp_lv_adapter_register_touch(&tp_cfg)) return NULL;
    bsp_display_brightness_init();
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
    lv_obj_set_style_bg_color(lv_layer_bottom(), lv_color_black(), 0);
    prefs_load();
    prefs_apply();
    ui_home();
    lv_timer_create(sleep_timer, 500, NULL);
    ui_unlock();
}

bool ui_screen_on(void) { return !s_asleep; }

void ui_prepare_deep_sleep(void)
{
    if (s_panel) esp_lcd_panel_disp_sleep(s_panel, true);
    tts_stop();
}
