#include "ui.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "bsp/display.h"
#include "bsp/esp-bsp.h"
#include "bsp/touch.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "net.h"

static const char *TAG = "ui";
LV_FONT_DECLARE(font_text_22)
LV_FONT_DECLARE(font_poem_32)
lv_font_t g_font_text, g_font_poem;

static void (*s_back)(void);
static void (*s_talk)(bool);
static lv_obj_t *s_home_status;

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
    s_home_status = NULL;
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

void ui_boot_button(bool pressed)
{
    if (!ui_lock()) return;
    if (!pressed && s_back) s_back();
    ui_unlock();
}

void ui_talk_button(bool pressed)
{
    if (!ui_lock()) return;
    if (s_talk) {
        s_talk(pressed);
    } else if (pressed && !s_back) {
        ui_chat();   /* on the home screen, PWR goes to DeepSeek */
    }
    ui_unlock();
}

/* ---- Home ---- */

static void update_home(void)
{
    if (!s_home_status) return;
    char buf[64];
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    if (net_time_valid()) {
        snprintf(buf, sizeof(buf), "%s  %02d:%02d", net_up() ? LV_SYMBOL_WIFI : "没有网络", tm.tm_hour, tm.tm_min);
    } else {
        snprintf(buf, sizeof(buf), "%s", net_up() ? LV_SYMBOL_WIFI : "连接网络中…");
    }
    lv_label_set_text(s_home_status, buf);
}

static void home_timer(lv_timer_t *t) { update_home(); }

static void go_chat(lv_event_t *e) { ui_chat(); }
static void go_poem(lv_event_t *e) { ui_poem_home(); }

void ui_home(void)
{
    lv_obj_t *scr = ui_screen();
    lv_obj_t *status = ui_label(scr, &g_font_text, C_DIM, "");
    lv_obj_align(status, LV_ALIGN_CENTER, 0, -176);

    lv_obj_t *b = ui_button(scr, "DeepSeek", &g_font_poem, 280, 104, C_BLUE, go_chat, NULL);
    lv_obj_align(b, LV_ALIGN_CENTER, 0, -62);
    b = ui_button(scr, "古诗", &g_font_poem, 280, 104, C_ACCENT, go_poem, NULL);
    lv_obj_align(b, LV_ALIGN_CENTER, 0, 66);

    ui_show(scr, NULL, NULL);
    s_home_status = status;
    update_home();
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
    lv_display_t *disp = display_start();
    if (!disp) {
        ESP_LOGE(TAG, "display start failed");
        return;
    }
    ui_lock();
    lv_obj_set_style_bg_color(lv_layer_bottom(), lv_color_black(), 0);
    ui_home();
    lv_timer_create(home_timer, 5000, NULL);
    ui_unlock();
}
