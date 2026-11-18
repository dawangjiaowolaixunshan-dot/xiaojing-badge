/*
 * 口算: a question on the screen, hold PWR and say the answer. Right, and the
 * next one comes by itself; wrong once, try again; wrong twice, it shows the
 * answer and moves on. Ten a round; 9+ right goes up a level, 5 or fewer goes
 * down, and the next round starts by itself. Each round feeds 小鲸.
 */
#include <stdio.h>
#include <string.h>

#include "asr.h"
#include "care.h"
#include "esp_random.h"
#include "mathq.h"
#include "net.h"
#include "pet.h"
#include "settings.h"
#include "tts.h"
#include "ui.h"

LV_FONT_DECLARE(font_poem_44)

#define ROUND 10
#define C_OK 0x5ad17a
#define C_MISS 0xff6b6b
#define C_TODO 0x3a4256
#define HINT "按住 PWR 说出答案"

typedef struct {
    uint8_t level, best;
    uint16_t rounds;
} math_save_t;

static lv_obj_t *s_scr, *s_whale, *s_level, *s_q, *s_result, *s_hint, *s_dots[ROUND];
static lv_font_t s_big;
static math_save_t s_save;
static math_q_t s_cur;
static int s_pos, s_right, s_tries;
static bool s_listening, s_busy, s_round_over;
static uint32_t s_next_at, s_asked_at;
static pet_clip_t s_after;

static unsigned rnd(void) { return esp_random(); }

static void show_level(void)
{
    char buf[64];
    snprintf(buf, sizeof(buf), "第 %d 关 · %s", s_save.level + 1, math_level_name(s_save.level));
    lv_label_set_text(s_level, buf);
}

static void dot(int i, uint32_t color)
{
    lv_obj_set_style_bg_color(s_dots[i], lv_color_hex(color), 0);
}

static void ask(void)
{
    math_make(s_save.level, &s_cur, rnd);
    char buf[40];
    snprintf(buf, sizeof(buf), "%s ?", s_cur.text);
    lv_label_set_text(s_q, buf);
    lv_obj_set_style_text_color(s_q, lv_color_hex(0xffffff), 0);
    lv_label_set_text(s_result, "");
    lv_label_set_text(s_hint, HINT);
    dot(s_pos, 0xe0e6f0);
    s_tries = 0;
    s_next_at = 0;
    s_asked_at = lv_tick_get();
}

static void new_round(void)
{
    s_pos = s_right = 0;
    s_round_over = false;
    for (int i = 0; i < ROUND; i++) dot(i, C_TODO);
    show_level();
    ask();
}

static void end_round(void)
{
    s_round_over = true;
    s_save.rounds++;
    char buf[96], say[96];
    int old = s_save.level;
    if (s_right >= 9 && s_save.level + 1 < MATH_LEVELS) s_save.level++;
    else if (s_right <= 5 && s_save.level > 0) s_save.level--;
    if (s_save.level > s_save.best) s_save.best = s_save.level;
    settings_set_blob("math", &s_save, sizeof(s_save));

    snprintf(buf, sizeof(buf), "%d 道答对", s_right);
    lv_label_set_text(s_q, buf);
    lv_obj_set_style_text_color(s_q, lv_color_hex(s_right >= 9 ? C_OK : 0xffffff), 0);
    if (s_save.level > old) {
        snprintf(buf, sizeof(buf), "过关！升到第 %d 关", s_save.level + 1);
        snprintf(say, sizeof(say), "答对%d道，过关啦！下一关：%s。", s_right, math_level_name(s_save.level));
    } else if (s_save.level < old) {
        snprintf(buf, sizeof(buf), "回到第 %d 关再练练", s_save.level + 1);
        snprintf(say, sizeof(say), "答对%d道，我们回上一关再练练。", s_right);
    } else {
        snprintf(buf, sizeof(buf), "再来一轮，对 9 道就过关");
        snprintf(say, sizeof(say), "答对%d道，再来一轮。", s_right);
    }
    lv_label_set_text(s_result, buf);
    lv_obj_set_style_text_color(s_result, lv_color_hex(C_ACCENT), 0);
    lv_label_set_text(s_hint, "下一轮马上开始");
    care_feed(5 + s_right);
    static const pet_clip_t EAT[] = {PET_EAT_RICE, PET_EAT_SNACK, PET_EAT_MELON};
    pet_play(s_whale, s_right >= 9 ? PET_CHEER : PET_JOY, false);
    s_after = EAT[esp_random() % 3];
    tts_say(say);
    s_next_at = lv_tick_get() + 6000;
}

static void next(void)
{
    if (++s_pos >= ROUND) end_round();
    else ask();
}

static void on_heard(const char *text, bool final)
{
    if (!final) return;
    ui_lock();
    if (s_scr && !s_round_over && !s_next_at) {
        s_busy = false;
        int got = text ? math_parse_answer(text) : -1;
        char buf[96];
        if (!text) {
            lv_label_set_text(s_result, "没连上语音识别，再说一次");
            lv_obj_set_style_text_color(s_result, lv_color_hex(C_MISS), 0);
            pet_play(s_whale, PET_IDLE, true);
        } else if (got == s_cur.answer) {
            uint32_t ms = lv_tick_get() - s_asked_at;
            snprintf(buf, sizeof(buf), "%s %d", s_cur.text, s_cur.answer);
            lv_label_set_text(s_q, buf);
            lv_obj_set_style_text_color(s_q, lv_color_hex(C_OK), 0);
            snprintf(buf, sizeof(buf), LV_SYMBOL_OK "  对了！%u.%u 秒", (unsigned)(ms / 1000), (unsigned)(ms % 1000 / 100));
            lv_label_set_text(s_result, buf);
            lv_obj_set_style_text_color(s_result, lv_color_hex(C_OK), 0);
            if (s_tries == 0) s_right++;
            dot(s_pos, s_tries == 0 ? C_OK : 0xe0b84a);
            static const pet_clip_t YAY[] = {PET_JOY, PET_GIGGLE, PET_SPIN, PET_DANCE};
            pet_play(s_whale, YAY[esp_random() % 4], false);
            s_after = PET_IDLE;
            s_next_at = lv_tick_get() + 1200;
        } else if (++s_tries < 2) {
            if (got < 0) snprintf(buf, sizeof(buf), "听到：%s\n没听到数字，再说一次", text[0] ? text : "（没听清）");
            else snprintf(buf, sizeof(buf), "%d 不对哦，再想想", got);
            lv_label_set_text(s_result, buf);
            lv_obj_set_style_text_color(s_result, lv_color_hex(C_MISS), 0);
            if (got < 0) s_tries--;   /* not heard is not a wrong answer */
            pet_play(s_whale, PET_SIGH, false);
            s_after = PET_IDLE;
        } else {
            snprintf(buf, sizeof(buf), "%s %d", s_cur.text, s_cur.answer);
            lv_label_set_text(s_q, buf);
            lv_obj_set_style_text_color(s_q, lv_color_hex(C_ACCENT), 0);
            snprintf(buf, sizeof(buf), "%d 不对，答案是 %d", got, s_cur.answer);
            lv_label_set_text(s_result, buf);
            lv_obj_set_style_text_color(s_result, lv_color_hex(C_MISS), 0);
            dot(s_pos, C_MISS);
            pet_play(s_whale, PET_SIGH, false);
            s_after = PET_IDLE;
            s_next_at = lv_tick_get() + 3000;
        }
    }
    ui_unlock();
}

static void math_talk(bool down)
{
    if (!s_scr || s_round_over || s_next_at) return;
    if (down) {
        if (s_listening || s_busy) return;
        if (!net_up()) {
            lv_label_set_text(s_result, "没有连上网络");
            return;
        }
        tts_stop();
        if (!asr_begin_lang(on_heard, "\"zh\"")) return;
        s_listening = true;
        lv_label_set_text(s_hint, "正在听…说完松开");
        pet_play(s_whale, PET_LISTEN, true);
    } else if (s_listening) {
        s_listening = false;
        s_busy = true;
        asr_end();
        lv_label_set_text(s_hint, HINT);
        pet_play(s_whale, PET_THINK, true);
    }
}

static void math_timer(lv_timer_t *t)
{
    if (!s_scr) return;
    if (s_next_at && lv_tick_get() >= s_next_at) {
        s_next_at = 0;
        if (s_round_over) new_round();
        else next();
    }
    if (s_whale && pet_finished(s_whale)) {
        pet_clip_t n = s_after;
        s_after = PET_IDLE;
        pet_play(s_whale, n, n == PET_IDLE);
    }
}

static void on_delete(lv_event_t *e)
{
    if (lv_event_get_target(e) != s_scr) return;
    tts_stop();
    if (s_listening) asr_cancel();
    s_listening = s_busy = false;
    s_scr = s_whale = NULL;
}

static void math_back(void) { ui_home(); }

void ui_math(void)
{
    if (!settings_get_blob("math", &s_save, sizeof(s_save)) || s_save.level >= MATH_LEVELS)
        memset(&s_save, 0, sizeof(s_save));
    s_big = font_poem_44;
    s_scr = ui_screen();
    lv_obj_add_event_cb(s_scr, on_delete, LV_EVENT_DELETE, NULL);
    s_whale = pet_create(s_scr);
    lv_image_set_scale(s_whale, 128);
    lv_obj_align(s_whale, LV_ALIGN_CENTER, 0, -192);
    pet_play(s_whale, PET_WAVE, false);
    s_after = PET_IDLE;

    s_level = ui_label(s_scr, &g_font_text, C_DIM, "");
    lv_obj_align(s_level, LV_ALIGN_CENTER, 0, -128);
    for (int i = 0; i < ROUND; i++) {
        s_dots[i] = lv_obj_create(s_scr);
        lv_obj_remove_style_all(s_dots[i]);
        lv_obj_set_size(s_dots[i], 12, 12);
        lv_obj_set_style_radius(s_dots[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(s_dots[i], LV_OPA_COVER, 0);
        lv_obj_align(s_dots[i], LV_ALIGN_CENTER, (i - 4) * 22 - 11, -92);
    }
    s_q = ui_label(s_scr, &s_big, 0xffffff, "");
    lv_obj_align(s_q, LV_ALIGN_CENTER, 0, -20);
    s_result = ui_label(s_scr, &g_font_text, C_OK, "");
    lv_obj_set_width(s_result, 340);
    lv_label_set_long_mode(s_result, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(s_result, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_result, LV_ALIGN_CENTER, 0, 62);
    s_hint = ui_label(s_scr, &g_font_text, C_DIM, "");
    lv_obj_align(s_hint, LV_ALIGN_CENTER, 0, 140);

    new_round();
    static lv_timer_t *timer;
    if (!timer) timer = lv_timer_create(math_timer, 150, NULL);
    ui_show(s_scr, math_back, math_talk);
}

void ui_math_inject(const char *heard)   /* as if said aloud (testing over USB) */
{
    if (s_scr) {
        ui_unlock();
        on_heard(heard, true);
        ui_lock();
    }
}

int ui_math_answer(void) { return s_scr ? s_cur.answer : -1; }   /* for testing */
