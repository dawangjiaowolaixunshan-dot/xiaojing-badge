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
#include "net.h"
#include "settings.h"
#include "tts.h"
#include "ui.h"

static const char *TAG = "ui_chat";

static lv_obj_t *s_scr, *s_status, *s_box, *s_q, *s_a, *s_intro, *s_talk, *s_talk_lbl;
static bool s_listening;
static bool s_speaking;      /* this reply goes to the speaker too */
static size_t s_spoken;      /* how much of it has been handed over */
static lv_obj_t *s_voice_lbl, *s_whale;
LV_IMAGE_DECLARE(whale_img);
typedef enum { W_IDLE, W_LISTEN, W_THINK, W_SPEAK } whale_mode_t;
static whale_mode_t s_mode;

/* One-off moves on top of the mode's motion. */
typedef enum { ACT_NONE, ACT_HOP, ACT_TILT, ACT_TWIST, ACT_JOY } whale_act_t;
static whale_act_t s_act;
static uint32_t s_act_t0, s_next_idle_act;

/* Bubbles drifting up around her: a whale's breath. */
#define BUBBLES 6
static struct {
    lv_obj_t *o;
    float x, y, vy, life;
} s_bub[BUBBLES];
static uint32_t s_last_bubble;
static int last_dx, last_dy, last_rot, last_sx, last_sy;   /* what the whale's styles are set to */
static void start_act(whale_act_t a);
#define WHALE_PX 136
static char s_qbuf[1100];
static char *s_wrap;   /* wrapped copy for a label, PSRAM */
#define BOX_W 300
#define WRAP_CAP 6144

/*
 * LVGL breaks Chinese anywhere, so a full stop can start a line. Wrap here
 * instead: break before a character that would pass the width, unless it's
 * punctuation that mustn't start a line, which hangs into the margin.
 */
static const char *wrap(const char *src)
{
    static const char *NO_START = "，。、；：？！）》」』”’…—,.;:?!)%";
    if (!s_wrap) s_wrap = heap_caps_malloc(WRAP_CAP, MALLOC_CAP_SPIRAM);
    if (!s_wrap) return src;
    const int limit = BOX_W - 26;   /* the margin a hanging mark goes in */
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

static void set_status(const char *text, uint32_t color)
{
    if (!s_status) return;
    lv_label_set_text(s_status, text);
    lv_obj_set_style_text_color(s_status, lv_color_hex(color), 0);
}

static void scroll_down(void)
{
    lv_obj_update_layout(s_box);
    lv_obj_scroll_to_y(s_box, LV_COORD_MAX, LV_ANIM_OFF);
}

/* ---- the conversation in the box (lock held) ---- */

static lv_obj_t *add_text(const char *text, uint32_t color, bool centre)
{
    lv_obj_t *l = ui_label(s_box, &g_font_text, color, centre ? text : wrap(text));
    lv_obj_set_width(l, BOX_W);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    if (centre) lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    return l;
}

static void add_pair(const char *q, const char *a)
{
    if (s_intro) {
        lv_obj_delete(s_intro);
        s_intro = NULL;
    }
    snprintf(s_qbuf, sizeof(s_qbuf), "你：%s", q);
    s_q = add_text(s_qbuf, C_ACCENT, false);
    lv_obj_set_style_margin_top(s_q, 10, 0);
    s_a = add_text(a, C_TEXT, false);
}

static void add_divider(void)
{
    add_text("—— 新对话 ——", C_DIM, true);
}

/* A press that heard nothing leaves no empty pair behind. */
static void drop_pair(void)
{
    if (s_q) lv_obj_delete(s_q);
    if (s_a) lv_obj_delete(s_a);
    s_q = s_a = NULL;
}

static void add_saved(const char *q, const char *a, void *ctx)
{
    if (q) add_pair(q, a);
    else add_divider();
}

/* ---- callbacks from the network tasks: take the lock, and the screen may be gone ---- */

static void on_reply(const char *reply)
{
    size_t n = strlen(reply);
    if (s_speaking && n > s_spoken) {
        tts_feed(reply + s_spoken);
        s_spoken = n;
    }
    ui_lock();
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
    if (!error) start_act(ACT_JOY);
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
        snprintf(s_qbuf, sizeof(s_qbuf), "你：%s", text);
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
        snprintf(s_qbuf, sizeof(s_qbuf), "你：%s", text);
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
        if (!asr_begin(on_asr)) {
            set_status("稍等一下再说", C_DIM);
            return;
        }
        s_listening = true;
        s_mode = W_LISTEN;
        add_pair("", "");
        scroll_down();
        set_status("正在听…", C_GREEN);
        lv_obj_set_style_bg_color(s_talk, lv_color_hex(C_GREEN), 0);
        lv_label_set_text(s_talk_lbl, "松开发送");
    } else if (s_listening) {
        s_listening = false;
        s_mode = W_THINK;
        asr_end();
        set_status("识别中…", C_DIM);
        lv_obj_set_style_bg_color(s_talk, lv_color_hex(C_BLUE), 0);
        lv_label_set_text(s_talk_lbl, "按住说话");
    }
}

static void on_talk(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    talk(code == LV_EVENT_PRESSED);
}

static void on_new(lv_event_t *e)
{
    if (chat_busy() || s_listening) return;
    chat_reset();
    add_divider();
    s_q = s_a = NULL;
    scroll_down();
}

static void on_voice(lv_event_t *e)
{
    tts_set_enabled(!tts_enabled());
    lv_label_set_text(s_voice_lbl, tts_enabled() ? LV_SYMBOL_VOLUME_MAX : LV_SYMBOL_MUTE);
}

static void on_delete(lv_event_t *e)
{
    tts_stop();
    if (s_listening) asr_cancel();
    s_listening = false;
    s_scr = s_status = s_box = s_q = s_a = s_intro = s_talk = s_talk_lbl = s_voice_lbl = s_whale = NULL;
    for (int i = 0; i < BUBBLES; i++) {
        s_bub[i].o = NULL;
        s_bub[i].life = 0;
    }
}

static float frand(void) { return (esp_random() & 0xffff) / 65535.0f; }

static void start_act(whale_act_t a)
{
    s_act = a;
    s_act_t0 = lv_tick_get();
}

static void spawn_bubble(float x, float y)
{
    for (int i = 0; i < BUBBLES; i++) {
        if (s_bub[i].life > 0 || !s_bub[i].o) continue;
        int d = 6 + (int)(frand() * 10);
        lv_obj_set_size(s_bub[i].o, d, d);
        s_bub[i].x = x;
        s_bub[i].y = y;
        s_bub[i].vy = 1.2f + frand() * 1.3f;
        s_bub[i].life = 1.0f;
        lv_obj_remove_flag(s_bub[i].o, LV_OBJ_FLAG_HIDDEN);
        return;
    }
}

static void move_bubbles(void)
{
    for (int i = 0; i < BUBBLES; i++) {
        if (!s_bub[i].o || s_bub[i].life <= 0) continue;
        s_bub[i].y -= s_bub[i].vy;
        s_bub[i].x += sinf(s_bub[i].y / 9.0f) * 0.6f;
        s_bub[i].life -= 0.022f;
        if (s_bub[i].life <= 0 || s_bub[i].y < -228) {
            s_bub[i].life = 0;
            lv_obj_add_flag(s_bub[i].o, LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_align(s_bub[i].o, LV_ALIGN_CENTER, (int)s_bub[i].x, (int)s_bub[i].y);
        if (s_bub[i].life < 0.4f) lv_obj_set_style_opa(s_bub[i].o, (lv_opa_t)(255 * s_bub[i].life / 0.4f) & 0xe0, 0);
        else lv_obj_set_style_opa(s_bub[i].o, LV_OPA_COVER, 0);
    }
}

static void on_whale_tap(lv_event_t *e)
{
    start_act(ACT_HOP);
    for (int i = 0; i < 4; i++) spawn_bubble(-40 + frand() * 80, -170 + frand() * 30);
}

static void anim_timer(lv_timer_t *t)
{
    if (s_talk) lv_obj_set_style_outline_width(s_talk, s_listening ? 2 + mic_level() / 8 : 0, 0);
    if (!s_whale) return;
    if (s_mode == W_SPEAK && !chat_busy() && !tts_active()) s_mode = W_IDLE;
    uint32_t now = lv_tick_get();
    float ts = now / 1000.0f, pi2 = 6.2831853f;
    float dx = 0, dy = 0, rot = 0, sx = 1, sy = 1;

    switch (s_mode) {
    case W_IDLE:   /* floating; moving costs little, scaling/rotating costs a lot */
        dy = 3 * sinf(pi2 * ts / 3.0f);
        if (!s_next_idle_act) s_next_idle_act = now + 4000;
        if (s_act == ACT_NONE && now > s_next_idle_act) {
            static const whale_act_t IDLE_ACTS[] = {ACT_HOP, ACT_TILT, ACT_TWIST, ACT_TILT};
            start_act(IDLE_ACTS[esp_random() % 4]);
            s_next_idle_act = now + 5000 + (uint32_t)(frand() * 6000);
        }
        if (now - s_last_bubble > 2500 && frand() < 0.02f) {
            spawn_bubble(30 + frand() * 30, -200);
            s_last_bubble = now;
        }
        break;
    case W_LISTEN: {   /* leans in, wobbling with the voice */
        float v = mic_level() / 100.0f;
        sx = sy = 1.04f + 0.18f * v;
        rot = 25 * sinf(pi2 * ts / 0.7f) * v + 20;
        dy = -3;
        break;
    }
    case W_THINK:   /* sways and blows bubbles */
        rot = 50 * sinf(pi2 * ts / 1.4f);
        dx = 4 * sinf(pi2 * ts / 1.4f);
        if (now - s_last_bubble > 450) {
            spawn_bubble(28 + frand() * 24, -215);
            s_last_bubble = now;
        }
        break;
    case W_SPEAK: {   /* bounces with the loudness of what she's saying */
        static float lvl;
        lvl = lvl * 0.6f + 0.4f * (tts_active() ? tts_level() / 100.0f : 0.5f + 0.5f * fabsf(sinf(pi2 * ts / 0.5f)));
        dy = -9 * lvl;   /* only a move: the text streaming in needs the time */
        break;
    }
    }
    if (s_mode != W_IDLE) s_next_idle_act = now + 3000;

    /* One-off moves, each about a second. */
    float p = (now - s_act_t0) / 1000.0f;
    switch (s_act) {
    case ACT_NONE: break;
    case ACT_HOP:
        if (p < 0.12f) dy += 4 * p / 0.12f;                                    /* crouch */
        else if (p < 0.62f) dy -= 26 * sinf(3.14159f * (p - 0.12f) / 0.5f) - 4 * (1 - (p - 0.12f) / 0.5f);
        else if (p < 0.75f) dy += 3 * sinf(3.14159f * (p - 0.62f) / 0.13f);    /* land */
        else s_act = ACT_NONE;
        break;
    case ACT_TILT:   /* a curious head tilt, held a moment */
        if (p < 1.4f) rot += 120 * sinf(3.14159f * fminf(p / 1.4f, 1)) * (s_act_t0 & 1 ? 1 : -1);
        else s_act = ACT_NONE;
        break;
    case ACT_TWIST:   /* a little shimmy */
        if (p < 0.8f) { rot += 90 * sinf(pi2 * p * 3) * (1 - p / 0.8f); dx += 3 * sinf(pi2 * p * 3); }
        else s_act = ACT_NONE;
        break;
    case ACT_JOY:   /* answered: two quick hops and a burst of bubbles */
        if (p < 0.05f && now - s_last_bubble > 200) {
            for (int i = 0; i < 5; i++) spawn_bubble(-50 + frand() * 100, -150 - frand() * 30);
            s_last_bubble = now;
        }
        if (p < 0.9f) dy -= 14 * fabsf(sinf(3.14159f * p / 0.45f));
        else s_act = ACT_NONE;
        break;
    }

    /* Touch the styles only when something visible changed: each change is
     * a redraw, and a rotated or scaled one is a slow one. */
    int idx = (int)lroundf(dx), idy = (int)lroundf(dy);
    int irot = (int)lroundf(rot / 10) * 10;            /* whole degrees */
    int isx = (int)lroundf(sx * 32) * 8, isy = (int)lroundf(sy * 32) * 8;   /* 1/32 steps */
    if (idx != last_dx) lv_obj_set_style_translate_x(s_whale, last_dx = idx, 0);
    if (idy != last_dy) lv_obj_set_style_translate_y(s_whale, last_dy = idy, 0);
    if (irot != last_rot) lv_obj_set_style_transform_rotation(s_whale, last_rot = irot, 0);
    if (isx != last_sx) lv_obj_set_style_transform_scale_x(s_whale, last_sx = isx, 0);
    if (isy != last_sy) lv_obj_set_style_transform_scale_y(s_whale, last_sy = isy, 0);
    move_bubbles();
}

void ui_chat(void)
{
    lv_obj_t *scr = ui_screen();
    ui_back_button(scr);
    s_whale = lv_image_create(scr);
    lv_image_set_src(s_whale, &whale_img);
    lv_obj_align(s_whale, LV_ALIGN_CENTER, 0, -160);
    lv_obj_set_style_transform_pivot_x(s_whale, WHALE_PX / 2, 0);
    lv_obj_set_style_transform_pivot_y(s_whale, WHALE_PX, 0);   /* sways from her feet */
    s_mode = W_IDLE;
    s_act = ACT_NONE;
    s_next_idle_act = 0;
    last_dx = last_dy = last_rot = 0;   /* a new image starts untransformed */
    last_sx = last_sy = 256;
    lv_obj_add_flag(s_whale, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_whale, on_whale_tap, LV_EVENT_CLICKED, NULL);
    for (int i = 0; i < BUBBLES; i++) {
        lv_obj_t *b = lv_obj_create(scr);
        lv_obj_remove_style_all(b);
        lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(b, 2, 0);
        lv_obj_set_style_border_color(b, lv_color_hex(0xa8e6ff), 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(0x5cc8ff), 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_20, 0);
        lv_obj_add_flag(b, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(b, LV_OBJ_FLAG_CLICKABLE);
        s_bub[i].o = b;
        s_bub[i].life = 0;
    }
    s_status = ui_label(scr, &g_font_text, C_DIM, "按住说话");
    lv_obj_align(s_status, LV_ALIGN_CENTER, 0, -76);

    s_box = lv_obj_create(scr);
    lv_obj_remove_style_all(s_box);
    lv_obj_set_size(s_box, BOX_W, 186);
    lv_obj_align(s_box, LV_ALIGN_CENTER, 0, 36);
    lv_obj_set_flex_flow(s_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_box, 8, 0);
    lv_obj_set_scroll_dir(s_box, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_box, LV_SCROLLBAR_MODE_OFF);
    s_intro = add_text("按住下面的按钮或侧面的 PWR 键说话，松开就发给 DeepSeek。往上滑能看以前的问答。",
                       C_DIM, false);
    history_each(HISTORY_SHOW, add_saved, NULL);
    s_q = s_a = NULL;

    s_talk = ui_button(scr, NULL, &g_font_text, 190, 58, C_BLUE, NULL, NULL);
    lv_obj_align(s_talk, LV_ALIGN_CENTER, 0, 168);
    lv_obj_set_style_outline_color(s_talk, lv_color_hex(C_GREEN), 0);
    lv_obj_set_style_outline_opa(s_talk, LV_OPA_50, 0);
    s_talk_lbl = ui_label(s_talk, &g_font_text, 0xffffff, "按住说话");
    lv_obj_center(s_talk_lbl);
    lv_obj_add_event_cb(s_talk, on_talk, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_talk, on_talk, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(s_talk, on_talk, LV_EVENT_PRESS_LOST, NULL);

    lv_obj_t *n = ui_button(scr, LV_SYMBOL_REFRESH, &g_font_text, 52, 52, C_PANEL, on_new, NULL);
    lv_obj_align(n, LV_ALIGN_CENTER, 186, -96);

    lv_obj_t *v = ui_button(scr, NULL, &g_font_text, 52, 52, C_PANEL, on_voice, NULL);
    lv_obj_align(v, LV_ALIGN_CENTER, 186, 44);
    s_voice_lbl = ui_label(v, &g_font_text, 0xffffff, tts_enabled() ? LV_SYMBOL_VOLUME_MAX : LV_SYMBOL_MUTE);
    lv_obj_center(s_voice_lbl);

    lv_obj_add_event_cb(scr, on_delete, LV_EVENT_DELETE, NULL);
    static lv_timer_t *anim;   /* one for good; idle while the chat screen is gone */
    if (!anim) anim = lv_timer_create(anim_timer, 50, NULL);
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
