/*
 * DeepSeek: hold the button (or BOOT) and talk; Bailian turns it into text
 * as you speak, letting go sends it, and the answer streams in below.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "asr.h"
#include "chat.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "history.h"
#include "mic.h"
#include "pet.h"
#include "net.h"
#include "settings.h"
#include "tts.h"
#include "voices.h"
#include "ui.h"

static const char *TAG = "ui_chat";

static lv_obj_t *s_footer;   /* "new conversation", kept last in the box */
static lv_obj_t *s_scr, *s_status, *s_box, *s_q, *s_a, *s_intro;
static lv_span_t *s_sp_done, *s_sp_warm, *s_sp_hot, *s_sp_dot;
static bool s_listening;
static bool s_speaking;      /* this reply goes to the speaker too */
static size_t s_spoken;      /* how much of it has been handed over */
static lv_obj_t *s_voice_lbl, *s_whale;
typedef enum { W_IDLE, W_LISTEN, W_THINK, W_SPEAK } whale_mode_t;
static whale_mode_t s_mode;

/* Audio-synced progressive reveal state (protected by ui_lock()). */
#define SYNC_CAP 6144
static char *s_reply_buf;    /* full reply text received so far (PSRAM) */
static char *s_seg_done;     /* wrapped slice: already spoken (PSRAM) */
static char *s_seg_warm;     /* wrapped slice: just spoken 1-2 chars (PSRAM) */
static char *s_seg_hot;      /* wrapped slice: currently speaking 1-2 chars (PSRAM) */
static size_t s_reply_len;
static bool s_reply_done;    /* DeepSeek stream finished */
static bool s_reply_want_tts;/* this reply expects audio playback */
static bool s_reveal_all;    /* user tapped to show full text immediately */
static bool s_anim_on;       /* active progressive reveal in progress */
static int s_reveal_chars;   /* UTF-8 characters currently revealed (monotonic) */
static int s_last_drawn;     /* last revealed char count rendered */
static int s_last_phase;     /* breathing dot phase last rendered */
static uint32_t s_sync_t0;   /* fallback timer start (muted or TTS failed) */
static uint32_t s_tick_cnt;

/* A one-off clip (a tap, cheering, sighing) plays out before the mode's own. */
static bool s_oneshot;
static void play_once(pet_clip_t c);
static char s_qbuf[1100];
static char *s_wrap;   /* wrapped copy for a label, PSRAM */
#define BOX_W 320   /* the bezel covers the screen's very edge: keep clear of it */
#define WRAP_CAP 6144

/*
 * LVGL breaks Chinese anywhere, so a full stop can start a line. Wrap here
 * instead: break before a character that would pass the width, unless it's
 * punctuation that mustn't start a line, which hangs into the margin.
 */
static const char *wrap_to(const char *src, int width)
{
    static const char *NO_START = "，。、；：？！）》」』”’…—,.;:?!)%";
    if (!s_wrap) s_wrap = heap_caps_malloc(WRAP_CAP, MALLOC_CAP_SPIRAM);
    if (!s_wrap) return src;
    const int limit = width - 24;   /* the margin a hanging mark goes in */
    int x = 0;
    size_t o = 0;
    for (const char *p = src; *p && o + 8 < WRAP_CAP;) {
        if (*p == '\n') {
            s_wrap[o++] = *p++;
            x = 0;
            continue;
        }
        unsigned char c0 = (unsigned char)*p;
        uint32_t n = c0 < 0x80 ? 1 : c0 < 0xe0 ? 2 : c0 < 0xf0 ? 3 : 4, cp = c0 & (0xff >> (n + 1));
        if (n == 1) cp = c0;
        for (uint32_t k = 1; k < n; k++) {
            if (!p[k]) { s_wrap[o] = 0; return s_wrap; }   /* cut off mid-character */
            cp = (cp << 6) | ((unsigned char)p[k] & 0x3f);
        }
        char ch[5] = {0};
        memcpy(ch, p, n < 4 ? n : 4);
        int w = lv_font_get_glyph_width(&g_font_text, cp, 0);
        if (x > 0 && x + w > limit && !strstr(NO_START, ch)) {
            s_wrap[o++] = '\n';
            x = 0;
            if (cp == ' ') {
                p += n;
                continue;
            }
        }
        memcpy(s_wrap + o, p, n);
        o += n;
        x += w;
        p += n;
    }
    s_wrap[o] = '\0';
    return s_wrap;
}

#define BUBBLE_W (BOX_W + 20)   /* an answer's text: the box's width (wrap_to keeps 24 px for hanging marks) */
static const char *wrap(const char *src) { return wrap_to(src, BUBBLE_W); }

static void on_status_tap(lv_event_t *e)
{
    if (!net_up()) ui_wifi_scan();
}

/* What's happening, in a line under her: shown only while something is. */
static void set_status(const char *text, uint32_t color)
{
    if (!s_status) return;
    bool idle = !strcmp(text, "按住说话");
    lv_obj_set_flag(s_status, LV_OBJ_FLAG_HIDDEN, idle);
    if (idle) return;
    lv_label_set_text(s_status, text);
    lv_obj_set_style_text_color(s_status, lv_color_hex(color), 0);
}

static void keep_footer_last(void)
{
    if (s_footer) lv_obj_move_to_index(s_footer, -1);
}

/* To the newest message. (Scrolling to LV_COORD_MAX didn't move at all: new
 * messages arrived below the view and the screen seemed stuck.) */
static void scroll_down(void)
{
    if (!s_box) return;
    lv_obj_update_layout(s_box);
    int32_t below = lv_obj_get_scroll_bottom(s_box);
    if (below > 0) lv_obj_scroll_by(s_box, 0, -below, LV_ANIM_OFF);
}

/* ---- the conversation in the box (lock held) ---- */

/* A line across the box: a note in the middle (a divider, a hint). */
static lv_obj_t *add_text(const char *text, uint32_t color, bool centre)
{
    lv_obj_t *l = ui_label(s_box, &g_font_text, color, centre ? text : wrap_to(text, BOX_W));
    lv_obj_set_width(l, BOX_W);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    if (centre) lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    keep_footer_last();
    return l;
}

static void replay_text(const char *src)
{
    if (!src || !src[0] || s_listening || chat_busy()) return;
    if (!net_up()) {
        set_status("没连网 · 点此连 Wi-Fi", C_RED);
        return;
    }
    size_t len = strlen(src);
    char *clean = heap_caps_malloc(len + 1, MALLOC_CAP_SPIRAM);
    if (!clean) return;
    size_t o = 0;
    for (size_t i = 0; i < len; i++) {
        if (src[i] != '\n') clean[o++] = src[i];
    }
    clean[o] = '\0';
    if (o > 0) {
        s_mode = W_SPEAK;
        set_status("正在重播…", C_BLUE);
        tts_say(clean);
    }
    free(clean);
}

static void on_replay_tap(lv_event_t *e)
{
    if (lv_indev_get_scroll_obj(lv_indev_active())) return;
    lv_obj_t *lbl = lv_event_get_target(e);
    replay_text(lv_label_get_text(lbl));
}

/* A message, no bubble round it: a bubble's fill and padding took a fifth
 * of the circle's widest band. 小鲸's answers use the full width in white;
 * the child's questions are blue. Returns the label; its
 * parent is the full-width row. */
static lv_obj_t *add_bubble(const char *text, bool mine)
{
    lv_obj_t *row = lv_obj_create(s_box);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, BOX_W, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *b = lv_obj_create(row);   /* kept as a wrapper so callers can find the row */
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    /* Both full width, left-aligned: the colour says who; right-aligned,
     * a short question left the round screen's left half empty. */
    lv_obj_t *l = ui_label(b, &g_font_text, mine ? 0x7fb2ff : 0xf2f2f2, wrap(text));
    if (!mine) {
        lv_obj_add_flag(l, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(l, on_replay_tap, LV_EVENT_SHORT_CLICKED, NULL);
    }
    keep_footer_last();
    return l;
}

/* Smooth follow-scroll: glides upward as new lines appear, unless the user scrolled up to read history. */
static void scroll_follow(bool smooth)
{
    if (!s_box) return;
    lv_obj_update_layout(s_box);
    int32_t below = lv_obj_get_scroll_bottom(s_box);
    if (below > 0 && (!smooth || below <= 160)) {
        lv_obj_scroll_by(s_box, 0, -below, smooth ? LV_ANIM_ON : LV_ANIM_OFF);
    }
}

/* ---- audio-synced progressive reveal (lock held) ---- */

static bool ensure_sync_bufs(void)
{
    if (!s_reply_buf) s_reply_buf = heap_caps_calloc(1, SYNC_CAP, MALLOC_CAP_SPIRAM);
    if (!s_seg_done) s_seg_done = heap_caps_calloc(1, SYNC_CAP, MALLOC_CAP_SPIRAM);
    if (!s_seg_warm) s_seg_warm = heap_caps_calloc(1, SYNC_CAP, MALLOC_CAP_SPIRAM);
    if (!s_seg_hot) s_seg_hot = heap_caps_calloc(1, SYNC_CAP, MALLOC_CAP_SPIRAM);
    return s_reply_buf && s_seg_done && s_seg_warm && s_seg_hot;
}

static uint32_t utf8_step(const char *p, size_t *out_len)
{
    unsigned char c0 = (unsigned char)p[0];
    if (!c0) {
        *out_len = 0;
        return 0;
    }
    size_t n = c0 < 0x80 ? 1 : c0 < 0xe0 ? 2 : c0 < 0xf0 ? 3 : 4;
    uint32_t cp = n == 1 ? c0 : (c0 & (0xff >> (n + 1)));
    for (size_t k = 1; k < n; k++) {
        if (!p[k]) {
            *out_len = 0;
            return 0;
        }
        cp = (cp << 6) | ((unsigned char)p[k] & 0x3f);
    }
    *out_len = n;
    return cp;
}

/* Relative spoken duration weight of one character (punctuation pauses longer). */
static int char_weight(uint32_t cp)
{
    if (cp == 0x3002 || cp == 0xff01 || cp == 0xff1f || cp == '\n' || cp == '.' || cp == '!' || cp == '?') return 36;
    if (cp == 0xff0c || cp == 0x3001 || cp == 0xff1b || cp == 0xff1a || cp == ',' || cp == ';' || cp == ':') return 20;
    if (cp == 0x2026 || cp == 0x2014) return 18;   /* … — */
    if (cp == ' ' || cp == 0x201c || cp == 0x201d || cp == 0x2018 || cp == 0x2019 ||
        cp == 0x300a || cp == 0x300b || cp == 0xff08 || cp == 0xff09 || cp == '(' || cp == ')') return 4;
    return cp < 0x80 ? 5 : 10;
}

static void count_text_metrics(const char *src, int *out_chars, int *out_weight)
{
    int chars = 0, w = 0;
    for (const char *p = src; *p;) {
        size_t n = 0;
        uint32_t cp = utf8_step(p, &n);
        if (!n) break;
        if (cp != '\n') {
            chars++;
            w += char_weight(cp);
        }
        p += n;
    }
    *out_chars = chars;
    *out_weight = w;
}

static int chars_for_weight(const char *src, int want_weight)
{
    int chars = 0, acc = 0;
    for (const char *p = src; *p;) {
        size_t n = 0;
        uint32_t cp = utf8_step(p, &n);
        if (!n) break;
        if (cp != '\n') {
            acc += char_weight(cp);
            chars++;
            if (acc >= want_weight) break;
        }
        p += n;
    }
    return chars;
}

static void copy_span_slice(char *dst, size_t cap, const char *wrapped, int from_ch, int to_ch)
{
    if (to_ch <= from_ch) {
        dst[0] = '\0';
        return;
    }
    size_t o = 0;
    int idx = 0;
    for (const char *p = wrapped; *p;) {
        size_t n = 0;
        uint32_t cp = utf8_step(p, &n);
        if (!n) break;
        if (cp == '\n') {
            if (idx >= from_ch && idx < to_ch && idx > 0 && o + 1 < cap) dst[o++] = '\n';
            p += n;
            continue;
        }
        if (idx >= from_ch && idx < to_ch) {
            if (o + n < cap) {
                memcpy(dst + o, p, n);
                o += n;
            }
        }
        idx++;
        p += n;
        if (idx >= to_ch) break;
    }
    dst[o] = '\0';
}

/* Render current reveal state into the 4-span progressive answer widget. */
static void render_reveal(bool settle, bool smooth_scroll)
{
    if (!s_a || !s_sp_done || !s_sp_warm || !s_sp_hot || !s_sp_dot) return;
    if (!ensure_sync_bufs()) return;

    const char *wrapped = wrap_to(s_reply_buf, BOX_W);
    if (settle) {
        lv_style_set_text_color(lv_span_get_style(s_sp_done), lv_color_hex(0xf2f2f2));
        lv_span_set_text(s_sp_done, wrapped);
        lv_span_set_text(s_sp_warm, "");
        lv_span_set_text(s_sp_hot, "");
        lv_span_set_text(s_sp_dot, "");
        lv_spangroup_refresh(s_a);
        scroll_follow(smooth_scroll);
        return;
    }

    int k = s_reveal_chars;
    int hot_n = k >= 2 ? 2 : k;
    int warm_n = k >= 4 ? 2 : (k > hot_n ? k - hot_n : 0);
    int hot_from = k - hot_n;
    int warm_from = hot_from - warm_n;

    copy_span_slice(s_seg_done, SYNC_CAP, wrapped, 0, warm_from);
    copy_span_slice(s_seg_warm, SYNC_CAP, wrapped, warm_from, hot_from);
    copy_span_slice(s_seg_hot, SYNC_CAP, wrapped, hot_from, k);

    lv_style_set_text_color(lv_span_get_style(s_sp_done), lv_color_hex(0xbcc6d8));
    lv_style_set_text_color(lv_span_get_style(s_sp_warm), lv_color_hex(0xeef5ff));
    lv_style_set_text_color(lv_span_get_style(s_sp_hot), lv_color_hex(0x7ce8ff));

    int lvl = tts_level();
    static const uint32_t DOT_COLS[4] = {0x3d6d8c, 0x5da9d4, 0x7ce8ff, 0x5da9d4};
    int phase = (s_tick_cnt / 4) & 3;
    uint32_t dot_col = lvl > 35 ? 0x9cf0ff : DOT_COLS[phase];
    lv_style_set_text_color(lv_span_get_style(s_sp_dot), lv_color_hex(dot_col));

    lv_span_set_text(s_sp_done, s_seg_done);
    lv_span_set_text(s_sp_warm, s_seg_warm);
    lv_span_set_text(s_sp_hot, s_seg_hot);
    lv_span_set_text(s_sp_dot, k > 0 ? " ·" : "·");
    lv_spangroup_refresh(s_a);
    scroll_follow(smooth_scroll);
}

static void finish_reveal_now(void)
{
    if (!s_anim_on) return;
    int total_chars = 0, total_w = 0;
    if (s_reply_buf) count_text_metrics(s_reply_buf, &total_chars, &total_w);
    s_reveal_chars = total_chars;
    s_anim_on = false;
    render_reveal(true, true);
}

static void on_answer_tap(lv_event_t *e)
{
    if (lv_indev_get_scroll_obj(lv_indev_active())) return;
    lv_obj_t *sg = lv_event_get_target(e);
    if (sg == s_a && s_anim_on && s_reply_len > 0) {
        s_reveal_all = true;
    } else if (!s_anim_on && !s_listening && !chat_busy()) {
        lv_span_t *sp = lv_spangroup_get_child(sg, 0);
        const char *txt = sp ? lv_span_get_text(sp) : NULL;
        if ((!txt || !txt[0]) && sg == s_a) txt = s_reply_buf;
        replay_text(txt);
    }
}

static lv_obj_t *add_answer_span(void)
{
    lv_obj_t *row = lv_obj_create(s_box);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, BOX_W, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *b = lv_obj_create(row);   /* wrapper matching add_bubble() hierarchy */
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, BOX_W, LV_SIZE_CONTENT);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *sg = lv_spangroup_create(b);
    lv_obj_remove_style_all(sg);
    lv_obj_set_width(sg, BOX_W);
    lv_obj_set_height(sg, LV_SIZE_CONTENT);
    lv_spangroup_set_mode(sg, LV_SPAN_MODE_BREAK);
    lv_obj_set_style_text_font(sg, &g_font_text, 0);
    lv_obj_add_flag(sg, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(sg, on_answer_tap, LV_EVENT_SHORT_CLICKED, NULL);

    s_sp_done = lv_spangroup_add_span(sg);
    s_sp_warm = lv_spangroup_add_span(sg);
    s_sp_hot = lv_spangroup_add_span(sg);
    s_sp_dot = lv_spangroup_add_span(sg);
    lv_span_t *all[4] = {s_sp_done, s_sp_warm, s_sp_hot, s_sp_dot};
    for (int i = 0; i < 4; i++) {
        lv_style_set_text_font(lv_span_get_style(all[i]), &g_font_text);
        lv_span_set_text(all[i], "");
    }
    lv_style_set_text_color(lv_span_get_style(s_sp_done), lv_color_hex(0xc4cde0));
    lv_style_set_text_color(lv_span_get_style(s_sp_warm), lv_color_hex(0xeaf2ff));
    lv_style_set_text_color(lv_span_get_style(s_sp_hot), lv_color_hex(0x7ce8ff));
    lv_style_set_text_color(lv_span_get_style(s_sp_dot), lv_color_hex(0x5da9d4));
    lv_spangroup_refresh(sg);
    keep_footer_last();
    return sg;
}

static void add_pair(const char *q, const char *a)
{
    finish_reveal_now();
    if (s_intro) {
        lv_obj_delete(s_intro);
        s_intro = NULL;
    }
    s_q = add_bubble(q[0] ? q : "…", true);
    lv_obj_set_style_margin_top(lv_obj_get_parent(lv_obj_get_parent(s_q)), 6, 0);
    if (a && a[0]) {
        s_sp_done = s_sp_warm = s_sp_hot = s_sp_dot = NULL;
        s_a = add_bubble(a, false);
    } else {
        s_a = add_answer_span();
    }
}

static void add_divider(void)
{
    add_text("—— 新对话 ——", C_DIM, true);
}

/* A press that heard nothing leaves no empty pair behind. */
static void drop_pair(void)
{
    s_anim_on = false;
    s_sp_done = s_sp_warm = s_sp_hot = s_sp_dot = NULL;
    if (s_q) lv_obj_delete(lv_obj_get_parent(lv_obj_get_parent(s_q)));   /* the bubble's row */
    if (s_a) lv_obj_delete(lv_obj_get_parent(lv_obj_get_parent(s_a)));
    s_q = s_a = NULL;
}

static void add_saved(const char *q, const char *a, void *ctx)
{
    int *n = ctx;
    if (n && (*n)++ == 0) add_text(LV_SYMBOL_UP "  往上翻是以前的对话", C_DIM, true);
    if (q) add_pair(q, a);
    else add_divider();
}

/* ---- callbacks from the network tasks: take the lock, and the screen may be gone ---- */

static void on_search(void)
{
    ui_lock();
    if (s_scr) {
        set_status("正在搜索…", C_BLUE);
        s_mode = W_THINK;
    }
    ui_unlock();
}

static void on_reply(const char *reply)
{
    size_t n = strlen(reply);
    bool feed = false;
    size_t from = 0;
    ui_lock();
    bool here = s_scr != NULL;
    if (s_speaking && n > s_spoken) {
        feed = true;
        from = s_spoken;
        s_spoken = n;
    }
    if (here) {
        if (s_mode != W_SPEAK) {
            set_status(s_reply_want_tts ? "正在酝酿…" : "正在回答…", C_BLUE);
        }
        s_mode = W_SPEAK;
        if (s_a && ensure_sync_bufs()) {
            size_t copy = n < SYNC_CAP - 1 ? n : SYNC_CAP - 1;
            memcpy(s_reply_buf, reply, copy);
            s_reply_buf[copy] = '\0';
            s_reply_len = copy;
            if (!s_anim_on) {
                s_anim_on = true;
                s_sync_t0 = lv_tick_get();
                s_last_drawn = -1;
                s_last_phase = -1;
            }
        }
    }
    ui_unlock();
    if (feed) tts_feed(reply + from);
}

/* After the text is all in, she keeps "speaking" while the audio plays. */
static bool s_speaking_tail(void) { return tts_active(); }

/* A failure on a weak signal says so: that's the fix, not the program. */
static void net_error_status(const char *what)
{
    char buf[64];
    int rssi = net_rssi();
    if (!net_up()) snprintf(buf, sizeof(buf), "没连网 · 点此连 Wi-Fi");
    else if (rssi < -75) snprintf(buf, sizeof(buf), "信号弱(%d)，靠近路由器", rssi);
    else snprintf(buf, sizeof(buf), "%s出错了，再说一次", what);
    set_status(buf, C_RED);
}

static void on_reply_done(const char *error)
{
    ui_lock();
    bool spk = s_speaking;
    s_speaking = false;
    ui_unlock();
    if (spk) {
        if (error) tts_stop();
        else tts_finish();
    }
    ui_lock();
    s_reply_done = true;
    if (!s_scr) {
        ui_unlock();
        return;
    }
    if (error) {
        s_reveal_all = true;
        finish_reveal_now();
    }
    if (error || !s_speaking_tail()) s_mode = W_IDLE;
    play_once(error ? PET_SIGH : PET_CHEER);
    if (error && !strcmp(error, "连不上 DeepSeek")) net_error_status("DeepSeek");
    else if (error) set_status(error, C_RED);
    else if (!s_anim_on) set_status("按住说话", C_DIM);
    ui_unlock();
}

static void on_asr(const char *text, bool final)
{
    ui_lock();
    if (!s_scr) {
        ui_unlock();
        return;
    }
    if (!final) {
        snprintf(s_qbuf, sizeof(s_qbuf), "%s", text[0] ? text : "…");
        if (s_q) lv_label_set_text(s_q, wrap(s_qbuf));
        scroll_down();
    } else if (!text) {
        s_mode = W_IDLE;
        drop_pair();
        const char *err = asr_last_error();
        if (!net_up()) net_error_status("语音识别");
        else if (err && err[0]) set_status(err, C_RED);
        else net_error_status("语音识别");
    } else if (!text[0]) {
        s_mode = W_IDLE;
        drop_pair();
        set_status("没听清，再说一次", C_DIM);
    } else {
        snprintf(s_qbuf, sizeof(s_qbuf), "%s", text);
        if (s_q) lv_label_set_text(s_q, wrap(s_qbuf));
        s_spoken = 0;
        s_reply_len = 0;
        if (ensure_sync_bufs()) s_reply_buf[0] = '\0';
        s_reply_done = false;
        s_reveal_all = false;
        s_anim_on = false;
        s_reveal_chars = 0;
        s_last_drawn = -1;
        s_last_phase = -1;
        set_status("正在想…", C_BLUE);
        ui_unlock();
        bool spk = tts_begin();   /* may briefly wait for prior stop; keep UI unlocked */
        ui_lock();
        if (!s_scr) {
            if (spk) tts_stop();
            ui_unlock();
            return;
        }
        s_speaking = spk;
        s_reply_want_tts = spk;
        if (!chat_ask(text, on_reply, on_reply_done)) {
            if (s_speaking) {
                tts_stop();
                s_speaking = false;
                s_reply_want_tts = false;
            }
            s_mode = W_IDLE;
            drop_pair();
            set_status("上一个还没答完", C_DIM);
        }
    }
    ui_unlock();
}

/* ---- talking (lock held) ---- */

static bool have_key(key_id_t id)
{
    char *k = heap_caps_malloc(256, MALLOC_CAP_SPIRAM);
    bool ok = k && settings_get_key(id, k, 256);
    if (k) {
        memset(k, 0, 256);
        free(k);
    }
    return ok;
}

static void talk(bool down)
{
    if (down) {
        if (s_listening) return;
        if (chat_busy()) {
            set_status("正在等上一句回复…", C_DIM);
            return;
        }
        tts_stop();   /* talking over it cuts it off */
        if (!net_up()) {
            net_state_t st = net_state();
            if (st == NET_STATE_CONNECTING || st == NET_STATE_SCANNING) {
                set_status("正在连网，稍等几秒…", C_BLUE);
            } else {
                set_status("没连网 · 点此连 Wi-Fi", C_RED);
            }
            return;
        }
        if (!have_key(KEY_DEEPSEEK) || !have_key(KEY_BAILIAN)) {
            set_status("没设置密钥", C_RED);
            return;
        }
        mic_voice_begin();   /* who's speaking, from their pitch and timbre */
        if (!asr_begin(on_asr)) {
            set_status("稍等一下再说", C_DIM);
            return;
        }
        s_listening = true;
        s_mode = W_LISTEN;
        add_pair("", "");
        scroll_down();
        set_status("正在听… 松开发送", C_GREEN);
    } else if (s_listening) {
        s_listening = false;
        s_mode = W_THINK;
        asr_end();
        voice_sample_t vs;
        mic_voice_end(&vs);
        chat_set_speaker(voices_classify(&vs));
        set_status("识别中…", C_DIM);
    }
}




static void on_delete(lv_event_t *e)
{
    tts_stop();
    if (s_listening || asr_active()) asr_cancel();
    s_listening = false;
    s_speaking = false;
    s_reply_want_tts = false;
    s_anim_on = false;
    s_sp_done = s_sp_warm = s_sp_hot = s_sp_dot = NULL;
    s_footer = NULL;
    s_scr = s_status = s_box = s_q = s_a = s_intro = s_voice_lbl = s_whale = NULL;
}

static void play_once(pet_clip_t c)
{
    if (!s_whale) return;
    pet_play(s_whale, c, false);
    s_oneshot = true;
}

static void on_whale_tap(lv_event_t *e)
{
    if (tts_active() || s_anim_on) {
        tts_stop();
        finish_reveal_now();
        s_speaking = false;
        s_mode = W_IDLE;
        set_status("已停止朗读", C_DIM);
        play_once(PET_SHY);
        return;
    }
    static const pet_clip_t TAP[] = {PET_WAVE, PET_JOY, PET_SHY, PET_GIGGLE};
    play_once(TAP[esp_random() % 4]);
}

static void step_reveal(void)
{
    if (!s_anim_on || !s_a || !s_reply_buf) return;
    s_tick_cnt++;

    int total_chars = 0, total_w = 0;
    count_text_metrics(s_reply_buf, &total_chars, &total_w);

    if (s_reveal_all) {
        s_reveal_chars = total_chars;
        s_anim_on = false;
        render_reveal(true, true);
        if (s_reply_done) set_status("按住说话", C_DIM);
        return;
    }

    int target = s_reveal_chars;
    bool finished = false;
    uint32_t played = tts_played_ms();
    bool tts_stalled = s_reply_done && played == 0 && lv_tick_elaps(s_sync_t0) > 5000;
    bool use_tts = s_reply_want_tts && !tts_failed() && !tts_stalled;

    if (use_tts) {
        uint32_t total_ms = tts_total_ms();
        bool active = tts_active();

        if (!active && s_reply_done) {
            target = total_chars;
            finished = true;
        } else if (played == 0) {
            target = 0;
        } else {
            set_status("按住说话", C_DIM);
            uint32_t eff_ms = played + 130;
            int want_w;
            if (total_ms > 0 && s_reply_done) {
                want_w = eff_ms >= total_ms ? total_w : (int)((int64_t)total_w * eff_ms / total_ms);
            } else {
                want_w = (int)((int64_t)eff_ms * 45 / 1000);
            }
            target = chars_for_weight(s_reply_buf, want_w);
            if (target > total_chars) target = total_chars;
        }
    } else {
        set_status("按住说话", C_DIM);
        uint32_t dt = lv_tick_elaps(s_sync_t0);
        int want_w = (int)((int64_t)dt * 115 / 1000);
        target = chars_for_weight(s_reply_buf, want_w);
        if (target > total_chars) target = total_chars;
        if (s_reply_done && target >= total_chars) finished = true;
    }

    if (target > s_reveal_chars) {
        int diff = target - s_reveal_chars;
        int step = finished ? diff : (diff > 14 ? 3 : (diff > 5 ? 2 : 1));
        s_reveal_chars += step;
        if (s_reveal_chars > total_chars) s_reveal_chars = total_chars;
    }

    if (finished && s_reveal_chars >= total_chars) {
        s_anim_on = false;
        render_reveal(true, true);
        set_status("按住说话", C_DIM);
        return;
    }

    int phase = (s_tick_cnt / 4) & 3;
    if (s_reveal_chars != s_last_drawn || phase != s_last_phase) {
        bool grew = s_reveal_chars != s_last_drawn;
        s_last_drawn = s_reveal_chars;
        s_last_phase = phase;
        render_reveal(false, grew);
    }
}

static void anim_timer(lv_timer_t *t)
{
    if (!s_whale) return;
    step_reveal();
    if (s_mode == W_SPEAK && !chat_busy() && !tts_active() && !s_anim_on) {
        s_mode = W_IDLE;
        if (s_status && !lv_obj_has_flag(s_status, LV_OBJ_FLAG_HIDDEN) &&
            !strcmp(lv_label_get_text(s_status), "正在重播…")) {
            set_status("按住说话", C_DIM);
        }
    }
    /* Listening, thinking and talking cut a one-off short; idling waits for it. */
    if (s_oneshot && !pet_finished(s_whale) && s_mode == W_IDLE) return;
    s_oneshot = false;
    static const pet_clip_t FOR_MODE[] = {[W_IDLE] = PET_IDLE, [W_LISTEN] = PET_LISTEN,
                                          [W_THINK] = PET_THINK, [W_SPEAK] = PET_TALK};
    pet_clip_t want = FOR_MODE[s_mode];
    if (pet_clip(s_whale) != want || pet_finished(s_whale)) pet_play(s_whale, want, true);
}

void ui_chat(void)
{
    lv_obj_t *scr = ui_screen();
    /* The round screen's middle is its widest part: the conversation gets it
     * (340 x 300). She sits small in the top cap, the talk button in the
     * bottom one; back is BOOT or a swipe right, a new conversation is the
     * last thing in the list. */
    s_whale = pet_create(scr);
    lv_image_set_scale(s_whale, 128);   /* 156 px at 50% */
    lv_obj_align(s_whale, LV_ALIGN_CENTER, 0, -190);
    lv_obj_add_flag(s_whale, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_whale, on_whale_tap, LV_EVENT_CLICKED, NULL);
    ui_volume_button(scr);
    s_mode = W_IDLE;
    s_oneshot = false;
    pet_play(s_whale, PET_IDLE, true);
    s_status = ui_label(scr, &g_font_text, C_DIM, "");
    lv_obj_align(s_status, LV_ALIGN_CENTER, 0, -134);
    lv_obj_add_flag(s_status, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_status, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(s_status, 14);
    lv_obj_add_event_cb(s_status, on_status_tap, LV_EVENT_SHORT_CLICKED, NULL);

    s_box = lv_obj_create(scr);
    lv_obj_remove_style_all(s_box);
    /* No buttons: PWR talks, new conversations start by themselves, BOOT or
     * a swipe right goes back. The list runs to the bottom of the circle,
     * padded so the newest message sits above where it narrows. */
    lv_obj_set_size(s_box, BOX_W, 316);
    lv_obj_align(s_box, LV_ALIGN_CENTER, 0, 40);   /* -118..198: below the status line, never under it */
    lv_obj_set_flex_flow(s_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_box, 10, 0);
    lv_obj_set_style_pad_top(s_box, 4, 0);
    lv_obj_set_style_pad_bottom(s_box, 78, 0);   /* the newest line rests above where the circle narrows */
    lv_obj_set_scroll_dir(s_box, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_box, LV_SCROLLBAR_MODE_OFF);
    s_footer = NULL;
    s_intro = add_text("按住侧面的 PWR 键说话，松开就发给小鲸。", C_DIM, true);
    int saved = 0;
    history_each(HISTORY_SHOW, add_saved, &saved);
    s_q = s_a = NULL;
    s_voice_lbl = NULL;   /* reading aloud is switched in 设置 */

    lv_obj_add_event_cb(scr, on_delete, LV_EVENT_DELETE, NULL);
    chat_on_search(on_search);
    static lv_timer_t *anim;   /* one for good; idle while the chat screen is gone */
    if (!anim) anim = lv_timer_create(anim_timer, 60, NULL);
    s_scr = scr;
    ui_show(scr, ui_home, talk);
    scroll_down();
    ESP_LOGI(TAG, "chat screen");
}

void ui_chat_inject(const char *question)
{
    ui_lock();
    bool here = s_scr != NULL;
    if (here && !s_listening && !chat_busy()) add_pair("", "");
    ui_unlock();
    if (here) {
        on_asr(question, true);
    } else {
        s_spoken = 0;
        s_speaking = tts_begin();
        chat_ask(question, on_reply, on_reply_done);
    }
}

bool ui_chat_listening(void) { return s_listening; }
