/*
 * 古诗: one poem at a time, in random order. The lines scroll like a watch
 * list: the one in the middle (the round screen's widest row) is drawn large
 * and bright, the others smaller and fainter the further they are. Reading
 * aloud moves the middle line along with the voice; otherwise a swipe up or
 * down does. Swipe left for the next poem, right for the last.
 */
#include <stdio.h>
#include <string.h>

#include "esp_random.h"
#include "poems.h"
#include "care.h"
#include "prefs.h"
#include "tts.h"
#include "ui.h"

LV_FONT_DECLARE(font_poem_44)
static lv_font_t s_big;   /* 44 px, with the 32 px font's metrics fixed like the others */

#define LIST_W 420
#define LIST_H 276

static int *s_order;     /* a shuffle of all the poems */
static int s_pos;
static lv_obj_t *s_scr, *s_list, *s_lines[POEM_MAX_LINES], *s_read_lbl;
static int s_nlines, s_active = -1;
static bool s_reading, s_reading_started;

static void shuffle(void)
{
    int n = poems_count();
    if (!s_order) s_order = lv_malloc(n * sizeof(int));
    for (int i = 0; i < n; i++) s_order[i] = i;
    for (int i = n - 1; i > 0; i--) {
        int j = esp_random() % (i + 1), t = s_order[i];
        s_order[i] = s_order[j];
        s_order[j] = t;
    }
    s_pos = 0;
}

static void build(void);

/* Fade each line by how far it is from the middle of the list. */
static void restyle(void)
{
    if (!s_list) return;
    lv_area_t la;
    lv_obj_get_coords(s_list, &la);
    int mid = (la.y1 + la.y2) / 2;
    for (int i = 0; i < s_nlines; i++) {
        lv_area_t a;
        lv_obj_get_coords(s_lines[i], &a);
        int d = LV_ABS((a.y1 + a.y2) / 2 - mid);
        int opa = i == s_active ? 255 : 230 - d * 170 / (LIST_H / 2);
        lv_obj_set_style_text_opa(s_lines[i], (lv_opa_t)LV_CLAMP(50, opa, 255), 0);
    }
}

/* Make line i the big one and bring it to the middle. */
static void set_active(int i, lv_anim_enable_t anim)
{
    if (i < 0 || i >= s_nlines) return;
    if (s_active >= 0 && s_active < s_nlines) {
        lv_obj_set_style_text_font(s_lines[s_active], &g_font_poem, 0);
        lv_obj_set_style_text_color(s_lines[s_active], lv_color_hex(0xc9cfdb), 0);
    }
    s_active = i;
    lv_obj_set_style_text_font(s_lines[i], &s_big, 0);
    lv_obj_set_style_text_color(s_lines[i], lv_color_hex(0xffffff), 0);
    lv_obj_update_layout(s_list);
    lv_area_t la, a;
    lv_obj_get_coords(s_list, &la);
    lv_obj_get_coords(s_lines[i], &a);
    lv_obj_scroll_by_bounded(s_list, 0, (la.y1 + la.y2) / 2 - (a.y1 + a.y2) / 2, anim);
    restyle();
}

/* The line nearest the middle, after a swipe. */
static int centred_line(void)
{
    lv_area_t la;
    lv_obj_get_coords(s_list, &la);
    int mid = (la.y1 + la.y2) / 2, best = 0, best_d = 1 << 30;
    for (int i = 0; i < s_nlines; i++) {
        lv_area_t a;
        lv_obj_get_coords(s_lines[i], &a);
        int d = LV_ABS((a.y1 + a.y2) / 2 - mid);
        if (d < best_d) {
            best_d = d;
            best = i;
        }
    }
    return best;
}

static void on_scroll(lv_event_t *e) { restyle(); }

static void on_scroll_end(lv_event_t *e)
{
    if (s_reading) return;   /* the voice leads while reading */
    int i = centred_line();
    if (i != s_active) set_active(i, LV_ANIM_ON);
}

static void show_read(bool reading)
{
    s_reading = reading;
    if (s_read_lbl) lv_label_set_text(s_read_lbl, reading ? LV_SYMBOL_STOP "  停止" : LV_SYMBOL_VOLUME_MAX "  朗读");
}

static void stop_reading(void)
{
    if (s_reading) tts_stop();
    show_read(false);
}

static void step(int dir)
{
    stop_reading();
    int n = poems_count();
    s_pos += dir;
    if (s_pos >= n) shuffle();
    if (s_pos < 0) s_pos = 0;
    build();
}

static void on_next(lv_event_t *e) { step(1); }

/*
 * 朗读: the title, the poet, then the lines, each its own sentence (each ends
 * in 。), so the synthesiser's sentence index says which line is playing:
 * 0 is the title, 1 the poet, 2 + i line i.
 */
static void on_read(lv_event_t *e)
{
    if (s_reading) {
        stop_reading();
        return;
    }
    const poem_t *w = poems_get(s_order[s_pos]);
    char *text = lv_malloc(2048);
    if (!text) return;
    /* \x1e between sentences: each goes to the synthesiser on its own, and
     * its sentence index then says which line is playing. */
    int n = snprintf(text, 2048, "%s。\x1e%s %s。\x1e", w->title, w->dynasty, w->author);
    for (int i = 0; i < w->nlines && n < 1900; i++) {
        char line[96];
        strlcpy(line, w->lines[i], sizeof(line));
        size_t len = strlen(line);
        /* Drop the line's closing mark (UTF-8, 3 bytes) unless it's ？ or ！. */
        if (len >= 3) {
            const char *end = line + len - 3;
            if (!strcmp(end, "，") || !strcmp(end, "。") || !strcmp(end, "；") || !strcmp(end, "、") ||
                !strcmp(end, "：")) {
                line[len - 3] = '\0';
                n += snprintf(text + n, 2048 - n, "%s。\x1e", line);
                continue;
            }
        }
        n += snprintf(text + n, 2048 - n, "%s\x1e", line);
    }
    tts_say_with(PREFS_VOICES[g_prefs.poem_voice], text);
    lv_free(text);
    show_read(true);
    s_reading_started = false;
    set_active(0, LV_ANIM_ON);
}

static void poem_timer(lv_timer_t *t)
{
    if (!s_scr || !s_reading) return;
    int k = tts_sentence_now();
    if (k >= 0) s_reading_started = true;
    if (k >= 2 && k - 2 != s_active) set_active(k - 2, LV_ANIM_ON);
    if (s_reading_started && !tts_active()) {   /* finished: reading a poem through counts */
        show_read(false);
        care_feed(10);
    }
}

static void on_gesture(lv_event_t *e)
{
    lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());
    if (dir == LV_DIR_LEFT) step(1);
    else if (dir == LV_DIR_RIGHT) step(-1);
}

static lv_obj_t *pill(const char *text, uint32_t bg, int x, lv_event_cb_t cb, lv_obj_t **label)
{
    lv_obj_t *b = ui_button(s_scr, NULL, &g_font_text, 118, 46, bg, cb, NULL);
    lv_obj_align(b, LV_ALIGN_CENTER, x, 186);
    lv_obj_t *l = ui_label(b, &g_font_text, 0xffffff, text);
    lv_obj_center(l);
    if (label) *label = l;
    return b;
}

static void build(void)
{
    const poem_t *w = poems_get(s_order[s_pos]);
    char buf[96];
    lv_obj_clean(s_scr);
    s_list = s_read_lbl = NULL;
    ui_back_button(s_scr);

    size_t title_chars = strlen(w->title) / 3;
    lv_obj_t *t = ui_label(s_scr, title_chars <= 7 ? &g_font_poem : &g_font_text, C_ACCENT, w->title);
    lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(t, 260);
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(t, LV_ALIGN_CENTER, 0, -184);
    snprintf(buf, sizeof(buf), "%s · %s", w->dynasty, w->author);
    lv_obj_t *a = ui_label(s_scr, &g_font_text, 0x8fa3c8, buf);
    lv_obj_align(a, LV_ALIGN_CENTER, 0, -146);

    s_list = lv_obj_create(s_scr);
    lv_obj_remove_style_all(s_list);
    lv_obj_set_size(s_list, LIST_W, LIST_H);
    lv_obj_align(s_list, LV_ALIGN_CENTER, 0, 24);   /* below the poet line, above the buttons */
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_list, 10, 0);
    lv_obj_set_style_pad_ver(s_list, LIST_H / 2 - 22, 0);   /* the first and last lines can reach the middle */
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);
    lv_obj_set_scroll_snap_y(s_list, LV_SCROLL_SNAP_CENTER);
    lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(s_list, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(s_list, on_scroll, LV_EVENT_SCROLL, NULL);
    lv_obj_add_event_cb(s_list, on_scroll_end, LV_EVENT_SCROLL_END, NULL);
    s_nlines = w->nlines;
    for (int i = 0; i < s_nlines; i++) {
        s_lines[i] = ui_label(s_list, &g_font_poem, 0xc9cfdb, w->lines[i]);
        lv_obj_add_flag(s_lines[i], LV_OBJ_FLAG_SNAPPABLE);
    }
    s_active = -1;
    set_active(0, LV_ANIM_OFF);

    pill(LV_SYMBOL_VOLUME_MAX "  朗读", 0x2f5bea, -64, on_read, &s_read_lbl);
    pill("换一首", 0xd9823c, 64, on_next, NULL);
    show_read(false);
}

static void on_delete(lv_event_t *e)
{
    tts_stop();
    if (lv_event_get_target(e) == s_scr) {
        s_scr = s_list = s_read_lbl = NULL;
        s_reading = false;
    }
}

void ui_poem_home(void)
{
    if (!s_big.get_glyph_dsc) {
        s_big = font_poem_44;
        s_big.line_height = 52;   /* as for the 32 px font: the converter's max comes from a few tall marks */
        s_big.base_line = 12;
    }
    if (!s_order) {
        shuffle();
    } else if (++s_pos >= poems_count()) {   /* each visit opens on a new poem */
        shuffle();
    }
    s_scr = ui_screen();
    lv_obj_add_event_cb(s_scr, on_gesture, LV_EVENT_GESTURE, NULL);
    lv_obj_add_event_cb(s_scr, on_delete, LV_EVENT_DELETE, NULL);
    build();
    static lv_timer_t *timer;
    if (!timer) timer = lv_timer_create(poem_timer, 120, NULL);
    ui_show(s_scr, ui_home, NULL);
}

void ui_poem_read(void)   /* 朗读, over USB for testing */
{
    if (s_scr) on_read(NULL);
}

int ui_poem_current(void) { return s_order ? s_order[s_pos] : -1; }
