/*
 * 单词: today's ten words, one card at a time. Each card says itself (the
 * word, then its meaning); hold PWR and read it back; right, and the next
 * card comes by itself; wrong, it says it again. Ten done feeds 小鲸.
 */
#include <stdio.h>
#include <string.h>

#include "asr.h"
#include "care.h"
#include "esp_random.h"
#include "net.h"
#include "pet.h"
#include "tts.h"
#include "ui.h"
#include "words.h"

#define VOICE_EN "loongstella_v2"   /* reads English and Chinese */
#define C_OK 0x5ad17a
#define C_MISS 0xff6b6b

static lv_obj_t *s_scr, *s_whale, *s_count, *s_word, *s_mean, *s_result, *s_hint;
static int s_set[WORDS_PER_DAY], s_n, s_pos;
static bool s_listening, s_busy, s_wrong_once;
static uint32_t s_next_at;          /* lv_tick to move on at (0: not yet) */
static pet_clip_t s_after;

static void say_card(void)
{
    const word_t *w = words_get(s_set[s_pos]);
    char text[96], mean[48];
    strlcpy(mean, w->meaning, sizeof(mean));
    char *semi = strstr(mean, "；");
    if (semi) *semi = '\0';   /* the first meaning only, aloud */
    snprintf(text, sizeof(text), "%s。\x1e%s。", w->word, mean);
    tts_say_with(VOICE_EN, text);
}

static void show_done(void);

static void show_card(void)
{
    if (s_pos >= s_n) {
        show_done();
        return;
    }
    const word_t *w = words_get(s_set[s_pos]);
    char buf[32];
    snprintf(buf, sizeof(buf), "%d / %d", s_pos + 1, s_n);
    lv_label_set_text(s_count, buf);
    lv_label_set_text(s_word, w->word);
    lv_label_set_text(s_mean, w->meaning);
    lv_label_set_text(s_result, "");
    lv_label_set_text(s_hint, "按住 PWR 跟着读一遍");
    s_wrong_once = false;
    s_next_at = 0;
    say_card();
}

static void show_done(void)
{
    lv_label_set_text(s_count, "");
    lv_label_set_text(s_word, "Great!");
    char buf[64];
    snprintf(buf, sizeof(buf), "今天的 %d 个单词读完啦\n一共认识 %d 个了", s_n, words_known());
    lv_label_set_text(s_mean, s_n ? buf : "今天没有要背的单词了");
    lv_label_set_text(s_result, "");
    lv_label_set_text(s_hint, "");
    if (s_n) {
        care_feed(20);
        static const pet_clip_t EAT[] = {PET_EAT_RICE, PET_EAT_SNACK, PET_EAT_MELON};
        pet_play(s_whale, PET_CHEER, false);
        s_after = EAT[esp_random() % 3];
        tts_say("今天的单词读完了，真棒！");
    }
    s_n = 0;
}

static void on_heard(const char *text, bool final)
{
    if (!final) return;
    ui_lock();
    if (s_scr && s_pos < s_n) {
        s_busy = false;
        const word_t *w = words_get(s_set[s_pos]);
        if (!text) {
            lv_label_set_text(s_result, "没连上语音识别，再读一次");
            lv_obj_set_style_text_color(s_result, lv_color_hex(C_MISS), 0);
            pet_play(s_whale, PET_IDLE, true);
        } else if (words_match(w->word, text)) {
            lv_label_set_text(s_result, LV_SYMBOL_OK "  读对了！");
            lv_obj_set_style_text_color(s_result, lv_color_hex(C_OK), 0);
            words_mark(s_set[s_pos], !s_wrong_once);   /* right after a miss still counts as a miss */
            pet_play(s_whale, PET_JOY, false);
            s_after = PET_IDLE;
            s_next_at = lv_tick_get() + 1500;
        } else {
            char buf[96];
            snprintf(buf, sizeof(buf), "听到的是：%s\n再听一遍，跟着读", text[0] ? text : "（没听清）");
            lv_label_set_text(s_result, buf);
            lv_obj_set_style_text_color(s_result, lv_color_hex(C_MISS), 0);
            s_wrong_once = true;
            pet_play(s_whale, PET_IDLE, true);
            say_card();
        }
    }
    ui_unlock();
}

static void words_talk(bool down)
{
    if (!s_scr || s_pos >= s_n) return;
    if (down) {
        if (s_listening || s_busy) return;
        if (!net_up()) {
            lv_label_set_text(s_result, "没有连上网络");
            return;
        }
        tts_stop();
        if (!asr_begin_lang(on_heard, "\"en\"")) return;
        s_listening = true;
        lv_label_set_text(s_hint, "正在听…读完松开");
        pet_play(s_whale, PET_LISTEN, true);
    } else if (s_listening) {
        s_listening = false;
        s_busy = true;
        asr_end();
        lv_label_set_text(s_hint, "按住 PWR 跟着读一遍");
        pet_play(s_whale, PET_THINK, true);
    }
}

static void on_again(lv_event_t *e)
{
    if (s_pos < s_n) say_card();
}

static void on_skip(lv_event_t *e)
{
    if (s_listening || s_busy || s_pos >= s_n) return;
    tts_stop();
    words_mark(s_set[s_pos], false);   /* skipped: back tomorrow */
    s_pos++;
    show_card();
}

static void words_timer(lv_timer_t *t)
{
    if (!s_scr) return;
    if (s_next_at && lv_tick_get() >= s_next_at) {
        s_next_at = 0;
        s_pos++;
        show_card();
    }
    if (s_whale && pet_finished(s_whale)) {
        pet_clip_t next = s_after;
        s_after = PET_IDLE;
        pet_play(s_whale, next, next == PET_IDLE);
    }
}

static lv_obj_t *pill(const char *text, uint32_t bg, int x, lv_event_cb_t cb)
{
    lv_obj_t *b = ui_button(s_scr, NULL, &g_font_text, 112, 44, bg, cb, NULL);
    lv_obj_align(b, LV_ALIGN_CENTER, x, 188);
    lv_obj_t *l = ui_label(b, &g_font_text, 0xffffff, text);
    lv_obj_center(l);
    return b;
}

static void on_delete(lv_event_t *e)
{
    if (lv_event_get_target(e) != s_scr) return;
    tts_stop();
    if (s_listening) asr_cancel();
    s_listening = s_busy = false;
    s_scr = s_whale = NULL;
}

static void words_back(void) { ui_home(); }

void ui_words(void)
{
    s_scr = ui_screen();
    lv_obj_add_event_cb(s_scr, on_delete, LV_EVENT_DELETE, NULL);
    s_whale = pet_create(s_scr);
    lv_image_set_scale(s_whale, 128);
    lv_obj_align(s_whale, LV_ALIGN_CENTER, 0, -192);
    pet_play(s_whale, PET_IDLE, true);
    s_after = PET_IDLE;

    s_count = ui_label(s_scr, &g_font_text, C_DIM, "");
    lv_obj_align(s_count, LV_ALIGN_CENTER, 0, -128);
    s_word = ui_label(s_scr, &lv_font_montserrat_48, 0xffffff, "");
    lv_obj_align(s_word, LV_ALIGN_CENTER, 0, -64);
    s_mean = ui_label(s_scr, &g_font_text, C_ACCENT, "");
    lv_obj_set_style_text_align(s_mean, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_mean, LV_ALIGN_CENTER, 0, -6);
    s_result = ui_label(s_scr, &g_font_text, C_OK, "");
    lv_obj_set_width(s_result, 330);
    lv_label_set_long_mode(s_result, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(s_result, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_result, LV_ALIGN_CENTER, 0, 60);
    s_hint = ui_label(s_scr, &g_font_text, C_DIM, "");
    lv_obj_align(s_hint, LV_ALIGN_CENTER, 0, 132);
    pill(LV_SYMBOL_VOLUME_MAX "  再听", 0x2f5bea, -62, on_again);
    pill("跳过", C_PANEL, 62, on_skip);

    s_n = words_today(s_set, WORDS_PER_DAY);
    s_pos = 0;
    show_card();
    static lv_timer_t *timer;
    if (!timer) timer = lv_timer_create(words_timer, 150, NULL);
    ui_show(s_scr, words_back, words_talk);
}

void ui_words_inject(const char *heard)   /* as if read aloud (testing over USB) */
{
    if (s_scr) {
        ui_unlock();
        on_heard(heard, true);
        ui_lock();
    }
}
