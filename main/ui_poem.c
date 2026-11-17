/*
 * 古诗: one poem at a time, in random order. Swipe left or tap 换一首 for the
 * next, swipe right to go back. Long poems page six lines at a time.
 */
#include <stdio.h>
#include <string.h>

#include "esp_random.h"
#include "poems.h"
#include "ui.h"

#define ROWS 6

static int *s_order;     /* a shuffle of all the poems */
static int s_pos, s_page;
static lv_obj_t *s_scr;

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

static void step(int dir)
{
    int n = poems_count();
    s_pos += dir;
    if (s_pos >= n) shuffle();
    if (s_pos < 0) s_pos = 0;
    s_page = 0;
    build();
}

static void on_next(lv_event_t *e) { step(1); }

static void on_page(lv_event_t *e)
{
    int pages = (poems_get(s_order[s_pos])->nlines + ROWS - 1) / ROWS;
    s_page = (s_page + 1) % pages;
    build();
}

static void on_gesture(lv_event_t *e)
{
    lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());
    if (dir == LV_DIR_LEFT) step(1);
    else if (dir == LV_DIR_RIGHT) step(-1);
}

static void build(void)
{
    const poem_t *w = poems_get(s_order[s_pos]);
    char buf[96];
    lv_obj_clean(s_scr);
    ui_back_button(s_scr);

    /* Title: the big font when it fits across the top. */
    size_t title_chars = strlen(w->title) / 3;
    lv_obj_t *t = ui_label(s_scr, title_chars <= 8 ? &g_font_poem : &g_font_text, C_ACCENT, w->title);
    lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(t, 320);
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(t, LV_ALIGN_CENTER, 0, -160);

    snprintf(buf, sizeof(buf), "%s · %s", w->dynasty, w->author);
    lv_obj_t *a = ui_label(s_scr, &g_font_text, C_DIM, buf);
    lv_obj_align(a, LV_ALIGN_CENTER, 0, -120);

    int first = s_page * ROWS, k = w->nlines - first < ROWS ? w->nlines - first : ROWS;
    int y0 = k < ROWS ? 20 - (k - 1) * 21 : -72;
    for (int r = 0; r < k; r++) {
        lv_obj_t *l = ui_label(s_scr, &g_font_poem, C_TEXT, w->lines[first + r]);
        lv_obj_align(l, LV_ALIGN_CENTER, 0, y0 + r * 42);
    }

    int pages = (w->nlines + ROWS - 1) / ROWS;
    if (pages > 1) {
        snprintf(buf, sizeof(buf), "%d/%d", s_page + 1, pages);
        lv_obj_t *b = ui_button(s_scr, buf, &g_font_text, 64, 48, C_PANEL, on_page, NULL);
        lv_obj_align(b, LV_ALIGN_CENTER, 186, 40);
    }
    lv_obj_t *b = ui_button(s_scr, "换一首", &g_font_text, 140, 46, C_ACCENT, on_next, NULL);
    lv_obj_align(b, LV_ALIGN_CENTER, 0, 184);
}

static void on_delete(lv_event_t *e) { s_scr = NULL; }

void ui_poem_home(void)
{
    if (!s_order) {
        shuffle();
    } else if (++s_pos >= poems_count()) {   /* each visit opens on a new poem */
        shuffle();
    }
    s_scr = ui_screen();
    lv_obj_add_event_cb(s_scr, on_gesture, LV_EVENT_GESTURE, NULL);
    lv_obj_add_event_cb(s_scr, on_delete, LV_EVENT_DELETE, NULL);
    s_page = 0;
    build();
    ui_show(s_scr, ui_home, NULL);
}
