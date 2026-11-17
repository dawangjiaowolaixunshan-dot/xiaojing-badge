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
static bool s_listening;
static bool s_speaking;      /* this reply goes to the speaker too */
static size_t s_spoken;      /* how much of it has been handed over */
static lv_obj_t *s_voice_lbl, *s_whale;
typedef enum { W_IDLE, W_LISTEN, W_THINK, W_SPEAK } whale_mode_t;
static whale_mode_t s_mode;

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
    keep_footer_last();
    return l;
}

static void add_pair(const char *q, const char *a)
{
    if (s_intro) {
        lv_obj_delete(s_intro);
        s_intro = NULL;
    }
    s_q = add_bubble(q[0] ? q : "…", true);
    lv_obj_set_style_margin_top(lv_obj_get_parent(lv_obj_get_parent(s_q)), 6, 0);
    s_a = add_bubble(a, false);   /* empty until the answer starts */
}

static void add_divider(void)
{
    add_text("—— 新对话 ——", C_DIM, true);
}

/* A press that heard nothing leaves no empty pair behind. */
static void drop_pair(void)
{
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
    if (s_speaking && n > s_spoken) {
        tts_feed(reply + s_spoken);
        s_spoken = n;
    }
    ui_lock();
    if (s_mode != W_SPEAK) set_status("正在回答…", C_BLUE);
    s_mode = W_SPEAK;
    if (s_a) {
        lv_label_set_text(s_a, wrap(reply));
        scroll_down();
    }
    ui_unlock();
}

/* After the text is all in, she keeps "speaking" while the audio plays. */
static bool s_speaking_tail(void) { return tts_active(); }

/* A failure on a weak signal says so: that's the fix, not the program. */
static void net_error_status(const char *what)
{
    char buf[64];
    int rssi = net_rssi();
    if (!net_up()) snprintf(buf, sizeof(buf), "%s失败：没连上 Wi-Fi", what);
    else if (rssi < -75) snprintf(buf, sizeof(buf), "信号弱(%d)，靠近路由器", rssi);
    else snprintf(buf, sizeof(buf), "%s出错了，再说一次", what);
    set_status(buf, C_RED);
}

static void on_reply_done(const char *error)
{
    if (s_speaking) {
        if (error) tts_stop();
        else tts_finish();
        s_speaking = false;
    }
    ui_lock();
    if (error || !s_speaking_tail()) s_mode = W_IDLE;
    play_once(error ? PET_SIGH : PET_CHEER);
    if (error && !strcmp(error, "连不上 DeepSeek")) net_error_status("DeepSeek");
    else if (error) set_status(error, C_RED);
    else set_status("按住说话", C_DIM);
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
        net_error_status("语音识别");
    } else if (!text[0]) {
        s_mode = W_IDLE;
        drop_pair();
        set_status("没听清，再说一次", C_DIM);
    } else {
        snprintf(s_qbuf, sizeof(s_qbuf), "%s", text);
        if (s_q) lv_label_set_text(s_q, wrap(s_qbuf));
        s_spoken = 0;
        s_speaking = tts_begin();   /* connects while DeepSeek starts writing */
        if (chat_ask(text, on_reply, on_reply_done)) {
            set_status("正在想…", C_BLUE);
        } else if (s_speaking) {
            tts_stop();
            s_speaking = false;
        }
        else set_status("上一个还没答完", C_DIM);
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
        if (s_listening || chat_busy()) return;
        tts_stop();   /* talking over it cuts it off */
        if (!net_up()) {
            set_status("没有连上网络", C_RED);
            return;
        }
        if (!have_key(KEY_DEEPSEEK) || !have_key(KEY_BAILIAN)) {
            set_status("没设置密钥", C_RED);
            return;
        }
        mic_pitch_begin();   /* who's speaking, from their pitch */
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
        chat_set_speaker(voices_classify(mic_pitch_end()));
        set_status("识别中…", C_DIM);
    }
}




static void on_gesture(lv_event_t *e)
{
    if (lv_indev_get_gesture_dir(lv_indev_active()) == LV_DIR_RIGHT) ui_home();   /* a swipe right goes back */
}

static void on_delete(lv_event_t *e)
{
    tts_stop();
    if (s_listening) asr_cancel();
    s_listening = false;
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
    static const pet_clip_t TAP[] = {PET_WAVE, PET_JOY, PET_SHY, PET_GIGGLE};
    play_once(TAP[esp_random() % 4]);
}

static void anim_timer(lv_timer_t *t)
{
    if (!s_whale) return;
    if (s_mode == W_SPEAK && !chat_busy() && !tts_active()) s_mode = W_IDLE;
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
    s_mode = W_IDLE;
    s_oneshot = false;
    pet_play(s_whale, PET_IDLE, true);
    s_status = ui_label(scr, &g_font_text, C_DIM, "");
    lv_obj_align(s_status, LV_ALIGN_CENTER, 0, -134);
    lv_obj_add_flag(s_status, LV_OBJ_FLAG_HIDDEN);

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
    lv_obj_add_event_cb(scr, on_gesture, LV_EVENT_GESTURE, NULL);

    lv_obj_add_event_cb(scr, on_delete, LV_EVENT_DELETE, NULL);
    chat_on_search(on_search);
    static lv_timer_t *anim;   /* one for good; idle while the chat screen is gone */
    if (!anim) anim = lv_timer_create(anim_timer, 100, NULL);
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
