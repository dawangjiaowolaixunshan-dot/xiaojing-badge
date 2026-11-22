/*
 * 古诗: one poem at a time, in random order. The lines scroll like a watch
 * list: the one in the middle (the round screen's widest row) is drawn large
 * and bright, the others smaller and fainter the further they are. Reading
 * aloud moves the middle line along with the voice; otherwise a swipe up or
 * down does. Swipe left for the next poem, right for the last.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "net.h"
#include "poems.h"
#include "care.h"
#include "prefs.h"
#include "tts.h"
#include "ui.h"

static const char *TAG = "ui_poem";

LV_FONT_DECLARE(font_poem_44)
static lv_font_t s_big;   /* 44 px, crisp native bitmap font (used for short 5-char poems) */

#define LIST_W 316
#define LIST_H 216
#define ROW_H 52
#define ROW_GAP 6
#define ROW_STEP (ROW_H + ROW_GAP)

static int *s_order;     /* a shuffle of all the poems */
static int s_pos;
static lv_obj_t *s_scr, *s_list, *s_rows[POEM_MAX_LINES], *s_lines[POEM_MAX_LINES], *s_read_btn, *s_read_lbl;
static int s_nlines, s_active = -1;
static int16_t s_last_opa[POEM_MAX_LINES];
static bool s_reading, s_reading_started, s_prog_scroll, s_net_warn;
static int s_line_w[POEM_MAX_LINES + 2]; /* cumulative spoken weights: 0=title, 1=poet, 2+i=line i */
static int s_total_w;

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

/* True iOS underdamped spring curve: crisp whoosh launch, ~10% overshoot, silky settle. */
static int32_t ios_spring_path(const lv_anim_t *a)
{
    if (a->act_time <= 0) return a->start_value;
    if (a->act_time >= a->duration) return a->end_value;
    float u = (float)a->act_time / (float)a->duration;
    float p = 1.0f - expf(-5.4f * u) * (cosf(6.2f * u) + 0.45f * sinf(6.2f * u));
    return a->start_value + (int32_t)lroundf((float)(a->end_value - a->start_value) * p);
}

/* On pure black AMOLED, dimming RGB color directly matches alpha-fade visually while skipping double alpha blend. */
static lv_color_t poem_shade(int ease256)
{
    /* Blend smoothly from deep slate (0x242c3c) at edges to crisp pure white (0xffffff) at center. */
    int r = 0x22 + ((0xff - 0x22) * ease256 >> 8);
    int g = 0x2a + ((0xff - 0x2a) * ease256 >> 8);
    int b = 0x3b + ((0xff - 0x3b) * ease256 >> 8);
    return lv_color_make((uint8_t)r, (uint8_t)g, (uint8_t)b);
}

static void restyle(void)
{
    if (!s_list || s_nlines <= 0) return;
    int32_t sy = lv_obj_get_scroll_y(s_list);
    const int half = LIST_H / 2;
    for (int i = 0; i < s_nlines; i++) {
        int32_t target_y = i * ROW_STEP;
        int d = LV_ABS(target_y - sy);
        if (d > half + ROW_STEP && s_last_opa[i] == 0) continue;

        int clamped = d > half ? half : d;
        int lin = 256 - (clamped * 256) / half;     /* 256 at center -> 0 at edge */
        int ease = (lin * lin) >> 8;                /* iOS spotlight falloff */
        int q = (ease == 256 || ease == 0) ? ease : (ease & ~7);

        if (q != s_last_opa[i]) {
            s_last_opa[i] = (int16_t)q;
            lv_obj_set_style_text_color(s_lines[i], poem_shade(q), 0);
        }
    }
}

static void scroll_anim_cb(void *var, int32_t v)
{
    lv_obj_t *list = var;
    if (!list || list != s_list) return;
    s_prog_scroll = true;
    lv_obj_scroll_to_y(list, v, LV_ANIM_OFF);
    s_prog_scroll = false;
    restyle();
}

static void scroll_done_cb(lv_anim_t *a)
{
    int n, avg, mx;
    ui_frames_get(&n, &avg, &mx);
    if (n > 0) ESP_LOGI(TAG, "spring anim: %d frames, avg %d ms, max %d ms", n, avg / 1000, mx / 1000);
}

static void fade_anim_cb(void *var, int32_t v)
{
    lv_obj_set_style_opa((lv_obj_t *)var, (lv_opa_t)v, 0);
}

/* Snappy iOS spring glide to line i. */
static void set_active(int i, lv_anim_enable_t anim)
{
    if (!s_list || i < 0 || i >= s_nlines) return;
    s_active = i;
    int32_t target_y = i * ROW_STEP;
    lv_anim_delete(s_list, scroll_anim_cb);
    if (anim == LV_ANIM_ON) {
        int32_t cur_y = lv_obj_get_scroll_y(s_list);
        ESP_LOGI(TAG, "glide line %d (y %d -> %d)", i, (int)cur_y, (int)target_y);
        if (cur_y == target_y) {
            restyle();
            return;
        }
        ui_frames_reset();
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, s_list);
        lv_anim_set_exec_cb(&a, scroll_anim_cb);
        lv_anim_set_completed_cb(&a, scroll_done_cb);
        lv_anim_set_values(&a, cur_y, target_y);
        int32_t dist = LV_ABS(target_y - cur_y);
        uint32_t dur = (uint32_t)LV_CLAMP(460, 440 + dist / 2, 560);
        lv_anim_set_duration(&a, dur);
        lv_anim_set_path_cb(&a, ios_spring_path);
        lv_anim_start(&a);
    } else {
        s_prog_scroll = true;
        lv_obj_scroll_to_y(s_list, target_y, LV_ANIM_OFF);
        s_prog_scroll = false;
        restyle();
    }
}

/* The line nearest the middle, after a manual swipe. */
static int centred_line(void)
{
    if (!s_list || s_nlines <= 0) return 0;
    int32_t sy = lv_obj_get_scroll_y(s_list);
    int i = (sy + ROW_STEP / 2) / ROW_STEP;
    return LV_CLAMP(0, i, s_nlines - 1);
}

static void on_touch_press(lv_event_t *e)
{
    if (s_list) lv_anim_delete(s_list, scroll_anim_cb);
}

static void on_row_tap(lv_event_t *e)
{
    if (s_reading || lv_indev_get_scroll_obj(lv_indev_active())) return;
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    set_active(i, LV_ANIM_ON);
}

static void on_scroll(lv_event_t *e) { restyle(); }

static void on_scroll_end(lv_event_t *e)
{
    if (s_prog_scroll || s_reading) return;   /* only settle after user drag */
    int i = centred_line();
    set_active(i, LV_ANIM_ON);
}

static void show_read(bool reading)
{
    s_reading = reading;
    s_net_warn = false;
    if (s_read_btn) lv_obj_set_style_bg_color(s_read_btn, lv_color_hex(0x2f5bea), 0);
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
static int count_utf8_chars(const char *s)
{
    int n = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if ((*p & 0xc0) != 0x80) n++;
    }
    return n;
}

static bool ends_with(const char *s, const char *suf)
{
    size_t ls = strlen(s), lf = strlen(suf);
    return ls >= lf && !memcmp(s + ls - lf, suf, lf);
}

static void on_read(lv_event_t *e)
{
    if (s_reading) {
        stop_reading();
        return;
    }
    if (!net_up()) {
        if (s_net_warn) {
            ui_wifi_scan();
            return;
        }
        s_net_warn = true;
        if (s_read_btn) lv_obj_set_style_bg_color(s_read_btn, lv_color_hex(C_RED), 0);
        if (s_read_lbl) lv_label_set_text(s_read_lbl, "点此连网");
        return;
    }
    const poem_t *w = poems_get(s_order[s_pos]);
    const char *voice = PREFS_VOICES[g_prefs.poem_voice];
    bool is_q3 = !strncmp(voice, "q3:", 3);
    char *text = lv_malloc(2048);
    if (!text) return;

    /* Precompute cumulative spoken weights so Qwen3 voices glide line-by-line in sync with audio. */
    int acc = count_utf8_chars(w->title) * 10 + 34;
    s_line_w[0] = acc;
    acc += (count_utf8_chars(w->dynasty) + count_utf8_chars(w->author)) * 10 + 38;
    s_line_w[1] = acc;
    for (int i = 0; i < w->nlines; i++) {
        int pause = (ends_with(w->lines[i], "，") || ends_with(w->lines[i], "、")) ? 18 : 34;
        acc += count_utf8_chars(w->lines[i]) * 10 + pause;
        s_line_w[2 + i] = acc;
    }
    s_total_w = acc;

    if (is_q3) {
        /* Feed the whole poem continuously with original punctuation so Qwen3 delivers natural cadence & emotion. */
        const char *dyn_suf = strlen(w->dynasty) == 3 ? "代" : "";
        int n = snprintf(text, 2048, "%s。%s%s，%s。\n", w->title, w->dynasty, dyn_suf, w->author);
        for (int i = 0; i < w->nlines && n < 1900; i++) {
            n += snprintf(text + n, 2048 - n, "%s\n", w->lines[i]);
        }
    } else {
        /* Legacy CosyVoice: \x1e splits sentences so server sentence-begin events drive line index. */
        int n = snprintf(text, 2048, "%s。\x1e%s %s。\x1e", w->title, w->dynasty, w->author);
        for (int i = 0; i < w->nlines && n < 1900; i++) {
            char line[96];
            strlcpy(line, w->lines[i], sizeof(line));
            size_t len = strlen(line);
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
    }
    tts_say_with(voice, text);
    lv_free(text);
    show_read(true);
    s_reading_started = false;
    set_active(0, LV_ANIM_ON);
}

static void poem_timer(lv_timer_t *t)
{
    if (!s_scr || !s_reading) return;
    if (tts_active()) s_reading_started = true;
    int k = tts_sentence_now();
    if (k >= 0) {
        s_reading_started = true;
    } else if (tts_played_ms() > 0 && s_total_w > 0) {
        /* Lead by 180 ms so the spring whoosh lands right as the next line begins. */
        uint32_t played = tts_played_ms() + 180;
        uint32_t total = tts_total_ms();
        int want_w = total > 0 ? (int)((int64_t)s_total_w * played / total)
                               : (int)((int64_t)played * 32 / 1000);
        int est = 0;
        while (est < s_nlines + 1 && s_line_w[est] <= want_w) est++;
        k = est;
    }
    if (k >= 2) {
        int line = k - 2;
        if (line >= s_nlines) line = s_nlines - 1;
        if (line > s_active) set_active(line, LV_ANIM_ON);
    }
    if (s_reading_started && !tts_active()) {   /* finished: reading a poem through counts */
        show_read(false);
        care_feed(10);
    }
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

static void slide_y_cb(void *var, int32_t v)
{
    lv_obj_set_style_translate_y((lv_obj_t *)var, v, 0);
}

static void build(void)
{
    const poem_t *w = poems_get(s_order[s_pos]);
    char buf[96];
    if (s_list) lv_anim_delete(s_list, NULL);
    lv_obj_clean(s_scr);
    s_list = s_read_btn = s_read_lbl = NULL;
    s_net_warn = false;
    ui_volume_button(s_scr);

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
    lv_obj_set_style_pad_row(s_list, ROW_GAP, 0);
    lv_obj_set_style_pad_top(s_list, (LIST_H - ROW_H) / 2, 0);
    lv_obj_set_style_pad_bottom(s_list, (LIST_H - ROW_H) / 2 + 16, 0); /* extra 16px headroom for spring overshoot */
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);
    lv_obj_set_scroll_snap_y(s_list, LV_SCROLL_SNAP_NONE);
    lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_event_cb(s_list, on_touch_press, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_list, on_scroll, LV_EVENT_SCROLL, NULL);
    lv_obj_add_event_cb(s_list, on_scroll_end, LV_EVENT_SCROLL_END, NULL);
    s_nlines = w->nlines;
    /* Use crisp 44 px bitmap font for 5-char poems, 32 px for longer lines: uniform per poem, 0 pop, 50+ FPS! */
    int max_ch = 0;
    for (int i = 0; i < s_nlines; i++) {
        int c = count_utf8_chars(w->lines[i]);
        if (c > max_ch) max_ch = c;
    }
    const lv_font_t *fnt = max_ch <= 6 ? &s_big : &g_font_poem;
    for (int i = 0; i < s_nlines; i++) {
        lv_obj_t *row = lv_obj_create(s_list);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, LIST_W, ROW_H);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_add_event_cb(row, on_row_tap, LV_EVENT_SHORT_CLICKED, (void *)(intptr_t)i);
        s_rows[i] = row;
        s_lines[i] = ui_label(row, fnt, 0xc9cfdb, w->lines[i]);
        lv_obj_center(s_lines[i]);
        s_last_opa[i] = -1;
    }
    s_active = -1;
    set_active(0, LV_ANIM_OFF);

    /* Crisp iOS spring slide-up + fade-in when switching poems. */
    lv_obj_set_style_opa(s_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_translate_y(s_list, 26, 0);
    lv_anim_t fa;
    lv_anim_init(&fa);
    lv_anim_set_var(&fa, s_list);
    lv_anim_set_exec_cb(&fa, fade_anim_cb);
    lv_anim_set_values(&fa, 0, 255);
    lv_anim_set_duration(&fa, 280);
    lv_anim_set_path_cb(&fa, lv_anim_path_ease_out);
    lv_anim_start(&fa);

    lv_anim_t sa;
    lv_anim_init(&sa);
    lv_anim_set_var(&sa, s_list);
    lv_anim_set_exec_cb(&sa, slide_y_cb);
    lv_anim_set_values(&sa, 26, 0);
    lv_anim_set_duration(&sa, 380);
    lv_anim_set_path_cb(&sa, ios_spring_path);
    lv_anim_start(&sa);

    s_read_btn = pill(LV_SYMBOL_VOLUME_MAX "  朗读", 0x2f5bea, -64, on_read, &s_read_lbl);
    pill("换一首", 0xd9823c, 64, on_next, NULL);
    show_read(false);
}

static void on_delete(lv_event_t *e)
{
    tts_stop();
    if (lv_event_get_target(e) == s_scr) {
        if (s_list) lv_anim_delete(s_list, NULL);
        lv_timer_t *rt = lv_display_get_refr_timer(NULL);
        if (rt) lv_timer_set_period(rt, 33);
        s_scr = s_list = s_read_btn = s_read_lbl = NULL;
        s_reading = false;
        s_net_warn = false;
    }
}

void ui_poem_home(void)
{
    if (!s_big.get_glyph_dsc) {
        s_big = font_poem_44;
        s_big.line_height = 52;
        s_big.base_line = 12;
    }
    if (!s_order) {
        shuffle();
    } else if (++s_pos >= poems_count()) {   /* each visit opens on a new poem */
        shuffle();
    }
    lv_timer_t *rt = lv_display_get_refr_timer(NULL);
    if (rt) lv_timer_set_period(rt, 16);   /* unlock 60 Hz refresh for silky iOS spring scroll */
    s_scr = ui_screen();
    lv_obj_add_event_cb(s_scr, on_delete, LV_EVENT_DELETE, NULL);
    build();
    static lv_timer_t *timer;
    if (!timer) timer = lv_timer_create(poem_timer, 40, NULL);
    ui_show(s_scr, ui_home, NULL);
}

void ui_poem_read(void)   /* 朗读, over USB for testing */
{
    if (s_scr) on_read(NULL);
}

int ui_poem_current(void) { return s_order ? s_order[s_pos] : -1; }
