#include "ui.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "bsp/display.h"
#include "bsp/esp-bsp.h"
#include "bsp/touch.h"
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
        esp_lcd_panel_draw_bitmap(s_te_panel, a->x1, y, a->x2 + 1, y + rows, s_bounce[k]);
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
    qsort(s_areas, s_nareas, sizeof(lv_area_t), cmp_area);
    xSemaphoreTake(s_te_sem, 0);                              /* a stale pulse */
    bool synced = xSemaphoreTake(s_te_sem, pdMS_TO_TICKS(30)) == pdTRUE;   /* none when the panel is off */
    int64_t t0 = s_te_at;
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
    for (int i = 0; i < 2; i++) xSemaphoreTake(s_buf_free, portMAX_DELAY);   /* all sent */
    for (int i = 0; i < 2; i++) xSemaphoreGive(s_buf_free);
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
    lv_display_add_event_cb(disp, on_refr, LV_EVENT_REFR_START, NULL);
    lv_display_add_event_cb(disp, on_refr, LV_EVENT_REFR_READY, NULL);

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
    if (esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "ui", &s_fast_lock) == ESP_OK) lv_timer_create(fast_timer, 30, NULL);
    ui_unlock();
}

bool ui_screen_on(void) { return !s_asleep; }

void ui_prepare_deep_sleep(void)
{
    if (s_panel) esp_lcd_panel_disp_sleep(s_panel, true);
    tts_stop();
}
