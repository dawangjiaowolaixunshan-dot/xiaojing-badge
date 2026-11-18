/*
 * Home: the whale girl in the middle saying a line now and then, the time
 * and date above her, battery round the rim, and three apps below: 小鲸
 * (DeepSeek), 古诗, 设置. Tap her to talk too.
 */
#include <math.h>
#include <stdio.h>
#include <time.h>

#include "esp_random.h"
#include "net.h"
#include "care.h"
#include "pet.h"
#include "pmu.h"
#include "ui.h"
#include "weather.h"

LV_IMAGE_DECLARE(whale_img);

static lv_obj_t *s_scr, *s_clock, *s_date, *s_arc, *s_whale, *s_dots[7];
static uint32_t s_next_extra;

static void update_dots(void)
{
    uint8_t week = care_week();
    for (int i = 0; i < 7; i++) {
        bool on = week >> (6 - i) & 1;   /* oldest on the left, today on the right */
        lv_obj_set_style_bg_opa(s_dots[i], on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    }
}

static void update_status(void)
{
    static const char *WEEK[] = {"日", "一", "二", "三", "四", "五", "六"};
    char buf[160];
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    if (net_time_valid()) {
        snprintf(buf, sizeof(buf), "%02d:%02d", tm.tm_hour, tm.tm_min);
        lv_label_set_text(s_clock, buf);
    } else {
        lv_label_set_text(s_clock, "--:--");
    }
    int pct;
    bool charging, usb;
    pmu_read(&pct, &charging, &usb);
    char batt[24] = "", wx[40] = "";
    if (charging) snprintf(batt, sizeof(batt), "  " LV_SYMBOL_CHARGE);   /* the level is the ring */
    weather_t w;
    if (weather_get(&w)) snprintf(wx, sizeof(wx), "  %s %.0f°", weather_desc(w.code), w.temp);
    if (net_time_valid()) {
        snprintf(buf, sizeof(buf), "%d月%d日 周%s%s%s%s", tm.tm_mon + 1, tm.tm_mday, WEEK[tm.tm_wday], wx, batt,
                 net_up() ? "" : "  无网络");
    } else {
        snprintf(buf, sizeof(buf), "%s%s", net_up() ? "对时中…" : "连接网络中…", batt);
    }
    lv_label_set_text(s_date, buf);
    update_dots();
    lv_arc_set_value(s_arc, pct >= 0 ? pct : 100);
    uint32_t col = pct < 0 || charging ? 0x4fc48c : pct < 15 ? 0xd9534f : pct < 30 ? 0xe0a045 : 0x4fc48c;
    lv_obj_set_style_arc_color(s_arc, lv_color_hex(col), LV_PART_INDICATOR);
}

static void home_timer(lv_timer_t *t)
{
    if (!s_scr) return;
    uint32_t now = lv_tick_get();
    static uint32_t last_status;
    if (now - last_status > 1000) {
        last_status = now;
        update_status();
    }
    /* Mostly breathing; every so often one of her other moves, then back. */
    if (pet_finished(s_whale) || (pet_clip(s_whale) == PET_IDLE && now > s_next_extra)) {
        if (pet_clip(s_whale) == PET_IDLE && now > s_next_extra) {
            static const pet_clip_t EXTRA[] = {PET_LOOK, PET_BUBBLES, PET_STRETCH, PET_SPIN, PET_DANCE,
                                               PET_FLOAT, PET_HUM, PET_LOOK, PET_BUBBLES};
            time_t tt = time(NULL);
            struct tm tm;
            localtime_r(&tt, &tm);
            bool late = net_time_valid() && (tm.tm_hour >= 21 || tm.tm_hour < 6);
            pet_clip_t c = late && esp_random() % 3 == 0 ? (esp_random() % 2 ? PET_YAWN : PET_SLEEP)
                                                          : EXTRA[esp_random() % (sizeof(EXTRA) / sizeof(EXTRA[0]))];
            if (care_food() < 30 && esp_random() % 2) c = PET_HUNGRY;   /* she lets you know */
            pet_play(s_whale, c, false);
            s_next_extra = now + 9000 + esp_random() % 9000;
        } else {
            pet_play(s_whale, PET_IDLE, true);
        }
    }
}

static void on_whale_tap(lv_event_t *e)
{
    static const pet_clip_t TAP[] = {PET_WAVE, PET_JOY, PET_SHY, PET_GIGGLE};
    pet_play(s_whale, TAP[esp_random() % 4], false);
    s_next_extra = lv_tick_get() + 9000;
}

static void go_chat(lv_event_t *e) { ui_chat(); }
static void go_poem(lv_event_t *e) { ui_poem_home(); }
static void go_settings(lv_event_t *e) { ui_settings(); }
static void go_recite(lv_event_t *e) { ui_recite(); }
static void go_words(lv_event_t *e) { ui_words(); }

static void on_delete(lv_event_t *e)
{
    if (lv_event_get_target(e) == s_scr) s_scr = NULL;   /* not a newer home made before this one went */
}

/* A round app button with a label under it, on a page of the dock. */
static lv_obj_t *app(lv_obj_t *pg, int x, uint32_t color, const char *label, lv_event_cb_t cb)
{
    lv_obj_t *b = lv_button_create(pg);
    lv_obj_set_size(b, 62, 62);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(color), 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x666666), LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(b, 24, 0);
    lv_obj_set_style_shadow_color(b, lv_color_hex(color), 0);
    lv_obj_set_style_shadow_opa(b, LV_OPA_60, 0);
    lv_obj_set_style_pad_all(b, 0, 0);
    lv_obj_align(b, LV_ALIGN_CENTER, x, -16);
    lv_obj_add_flag(b, LV_OBJ_FLAG_SCROLL_CHAIN_HOR);   /* a swipe that starts on a button still turns the page */
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = ui_label(pg, &g_font_text, 0xb8c2d8, label);
    lv_obj_align(l, LV_ALIGN_CENTER, x, 30);
    return b;
}

static lv_obj_t *page(lv_obj_t *dock)
{
    lv_obj_t *pg = lv_obj_create(dock);
    lv_obj_remove_style_all(pg);
    lv_obj_set_size(pg, 466, 112);
    lv_obj_add_flag(pg, LV_OBJ_FLAG_SNAPPABLE);
    lv_obj_remove_flag(pg, LV_OBJ_FLAG_SCROLLABLE);
    return pg;
}

static lv_obj_t *s_page_dots[2];

static void show_page_dot(int page)
{
    for (int i = 0; i < 2; i++)
        lv_obj_set_style_bg_color(s_page_dots[i], lv_color_hex(i == page ? 0xe0e6f0 : 0x3a4256), 0);
}

static void on_dock_scroll(lv_event_t *e)
{
    lv_obj_t *dock = lv_event_get_target(e);
    show_page_dot(lv_obj_get_scroll_x(dock) > 233 ? 1 : 0);
}

void ui_home(void)
{
    lv_obj_t *scr = ui_screen();

    s_arc = lv_arc_create(scr);
    lv_obj_set_size(s_arc, 458, 458);
    lv_obj_center(s_arc);
    lv_arc_set_rotation(s_arc, 270);
    lv_arc_set_bg_angles(s_arc, 0, 360);
    lv_arc_set_range(s_arc, 0, 100);
    lv_obj_remove_style(s_arc, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(s_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(s_arc, 5, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_arc, 5, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_arc, lv_color_hex(0x2a2a32), LV_PART_MAIN);

    s_clock = ui_label(scr, &g_font_clock, C_TEXT, "--:--");
    lv_obj_align(s_clock, LV_ALIGN_CENTER, 0, -176);
    s_date = ui_label(scr, &g_font_text, 0x8fa3c8, "");
    lv_obj_align(s_date, LV_ALIGN_CENTER, 0, -130);

    s_whale = pet_create(scr);
    lv_image_set_scale(s_whale, 270);   /* 156 px at 105%: she's the home screen */
    lv_obj_align(s_whale, LV_ALIGN_CENTER, 0, -26);

    /* The week's check-ins under her: a lit dot for each day with learning. */
    for (int i = 0; i < 7; i++) {
        int d = i == 6 ? 11 : 8;
        s_dots[i] = lv_obj_create(scr);
        lv_obj_remove_style_all(s_dots[i]);
        lv_obj_set_size(s_dots[i], d, d);
        lv_obj_set_style_radius(s_dots[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(s_dots[i], 1, 0);
        lv_obj_set_style_border_color(s_dots[i], lv_color_hex(0xd9823c), 0);
        lv_obj_set_style_bg_color(s_dots[i], lv_color_hex(0xd9823c), 0);
        lv_obj_align(s_dots[i], LV_ALIGN_CENTER, (i - 3) * 20, 72);
        lv_obj_remove_flag(s_dots[i], LV_OBJ_FLAG_CLICKABLE);
    }
    lv_obj_add_flag(s_whale, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_whale, on_whale_tap, LV_EVENT_CLICKED, NULL);   /* she reacts; 小鲸 below opens chat */
    pet_play(s_whale, PET_WAVE, false);   /* hello */

    /* The apps: pages of four, swiped sideways, a dot for each page below. */
    lv_obj_t *dock = lv_obj_create(scr);
    lv_obj_remove_style_all(dock);
    lv_obj_set_size(dock, 466, 112);
    lv_obj_align(dock, LV_ALIGN_CENTER, 0, 128);
    lv_obj_set_flex_flow(dock, LV_FLEX_FLOW_ROW);
    lv_obj_set_scroll_dir(dock, LV_DIR_HOR);
    lv_obj_set_scroll_snap_x(dock, LV_SCROLL_SNAP_CENTER);
    lv_obj_add_flag(dock, LV_OBJ_FLAG_SCROLL_ONE);
    lv_obj_set_scrollbar_mode(dock, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_event_cb(dock, on_dock_scroll, LV_EVENT_SCROLL_END, NULL);
    lv_obj_t *p1 = page(dock), *p2 = page(dock);

    lv_obj_t *b = app(p1, -138, 0x2f5bea, "小鲸", go_chat);
    lv_obj_t *icon = lv_image_create(b);
    lv_image_set_src(icon, &whale_img);
    lv_image_set_scale(icon, 110);   /* 136 px at 43%: 58 px */
    lv_obj_center(icon);
    lv_obj_remove_flag(icon, LV_OBJ_FLAG_CLICKABLE);
    b = app(p1, -46, 0xd9823c, "古诗", go_poem);
    lv_obj_center(ui_label(b, &g_font_poem, 0xffffff, "诗"));
    b = app(p1, 46, 0x2e9e6b, "背诗", go_recite);
    lv_obj_center(ui_label(b, &g_font_poem, 0xffffff, "背"));
    b = app(p1, 138, 0x8a5cf6, "单词", go_words);
    lv_obj_center(ui_label(b, &lv_font_montserrat_28, 0xffffff, "Aa"));
    b = app(p2, -138, 0x3a4256, "设置", go_settings);
    lv_obj_center(ui_label(b, &lv_font_montserrat_28, 0xffffff, LV_SYMBOL_SETTINGS));

    for (int i = 0; i < 2; i++) {
        s_page_dots[i] = lv_obj_create(scr);
        lv_obj_remove_style_all(s_page_dots[i]);
        lv_obj_set_size(s_page_dots[i], 6, 6);
        lv_obj_set_style_radius(s_page_dots[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(s_page_dots[i], LV_OPA_COVER, 0);
        lv_obj_align(s_page_dots[i], LV_ALIGN_CENTER, (i ? 6 : -6), 196);
    }
    show_page_dot(0);

    lv_obj_add_event_cb(scr, on_delete, LV_EVENT_DELETE, NULL);
    s_scr = scr;
    s_next_extra = lv_tick_get() + 8000;
    update_status();
    static lv_timer_t *timer;
    if (!timer) timer = lv_timer_create(home_timer, 200, NULL);
    ui_show(scr, NULL, NULL);
}
