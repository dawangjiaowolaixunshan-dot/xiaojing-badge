/*
 * 背诗: recite a poem to 小鲸. The lines start hidden (○ for each character,
 * punctuation shown); hold PWR and say the poem; on release each character
 * turns green (said) or red (missed), with a score and stars. A good try
 * feeds 小鲸 and checks the day in. 提示 shows each line's first character,
 * then the whole poem.
 */
#include <stdio.h>
#include <string.h>

#include "asr.h"
#include "care.h"
#include "esp_random.h"
#include "mic.h"
#include "net.h"
#include "pet.h"
#include "poems.h"
#include "recite.h"
#include "settings.h"
#include "tts.h"
#include "ui.h"

#define ROW_H 40
#define C_OK 0x5ad17a
#define C_MISS 0xff6b6b
#define C_HIDDEN 0x55607a

static lv_obj_t *s_scr, *s_whale, *s_status, *s_list, *s_rows[POEM_MAX_LINES];
static int s_poem = -1, s_hint;     /* hint: 0 hidden, 1 first characters, 2 all */
static bool s_listening, s_scored, s_busy;
static recite_result_t s_res;
static pet_clip_t s_after;          /* a clip to play when the current one-shot ends */

static bool is_punct_cp(int cp) { return !recite_is_hanzi(cp); }

/* One line as spans: hidden ○, a hint, or the scored colours. `base` is the
 * index of the line's first character in the poem's hanzi. */
static void render_line(int li, int *base)
{
    const poem_t *w = poems_get(s_poem);
    lv_obj_t *sg = s_rows[li];
    while (lv_spangroup_get_span_count(sg)) lv_spangroup_delete_span(sg, lv_spangroup_get_child(sg, 0));
    const char *p = w->lines[li];
    int k = 0, cp;
    char run[192] = "";
    uint32_t run_col = 0;
    size_t rn = 0;
    while (1) {
        const char *start = p;
        cp = recite_utf8_next(&p);
        uint32_t col = run_col;
        char piece[8] = "";
        if (cp) {
            if (is_punct_cp(cp)) {
                col = 0x8890a6;
                memcpy(piece, start, p - start);
            } else {
                int idx = *base + k++;
                bool show = s_scored || s_hint >= 2 || (s_hint == 1 && k == 1);
                if (s_scored) col = idx < RECITE_MAX && s_res.ok[idx] ? C_OK : C_MISS;
                else col = show ? C_TEXT : C_HIDDEN;
                if (show) memcpy(piece, start, p - start);
                else strcpy(piece, "○");
            }
        }
        /* flush the run when the colour changes or at the end */
        if (rn && (!cp || col != run_col)) {
            lv_span_t *sp = lv_spangroup_add_span(sg);
            lv_span_set_text(sp, run);
            lv_style_set_text_color(lv_span_get_style(sp), lv_color_hex(run_col));
            rn = 0;
            run[0] = '\0';
        }
        if (!cp) break;
        run_col = col;
        size_t pl = strlen(piece);
        if (rn + pl < sizeof(run)) {
            memcpy(run + rn, piece, pl + 1);
            rn += pl;
        }
    }
    *base += k;
    lv_spangroup_refresh(sg);
}

static void row_opa_cb(void *var, int32_t v)
{
    lv_obj_set_style_opa((lv_obj_t *)var, (lv_opa_t)v, 0);
}

static void animate_rows_cascade(void)
{
    const poem_t *w = poems_get(s_poem);
    for (int i = 0; i < w->nlines; i++) {
        lv_anim_delete(s_rows[i], row_opa_cb);
        lv_obj_set_style_opa(s_rows[i], LV_OPA_TRANSP, 0);
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, s_rows[i]);
        lv_anim_set_exec_cb(&a, row_opa_cb);
        lv_anim_set_values(&a, 0, 255);
        lv_anim_set_delay(&a, (uint32_t)(i * 55));
        lv_anim_set_duration(&a, 280);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
        lv_anim_start(&a);
    }
}

static void render(void)
{
    const poem_t *w = poems_get(s_poem);
    int base = 0;
    for (int i = 0; i < w->nlines; i++) render_line(i, &base);
}

static void set_status(const char *text, uint32_t color)
{
    lv_label_set_text(s_status, text);
    lv_obj_set_style_text_color(s_status, lv_color_hex(color), 0);
}

static void play_once(pet_clip_t c, pet_clip_t then)
{
    pet_play(s_whale, c, false);
    s_after = then;
}

/* ---- the result ---- */

static void finish(const char *heard)
{
    const poem_t *w = poems_get(s_poem);
    char joined[1024] = "";
    for (int i = 0; i < w->nlines; i++) strlcat(joined, w->lines[i], sizeof(joined));
    recite_score(joined, heard ? heard : "", &s_res);
    s_scored = true;
    render();
    animate_rows_cascade();
    int pct = s_res.n ? s_res.right * 100 / s_res.n : 0;
    int stars = pct >= 95 ? 3 : pct >= 80 ? 2 : pct >= 60 ? 1 : 0;
    char buf[80];
    snprintf(buf, sizeof(buf), "背对 %d/%d  %s%s%s", s_res.right, s_res.n, stars > 0 ? "★" : "☆", stars > 1 ? "★" : "☆",
             stars > 2 ? "★" : "☆");
    set_status(buf, stars ? C_ACCENT : C_DIM);
    static const char *SAY[] = {"没关系，先读几遍，再来背给我听。", "差一点点，红色的地方再看看。",
                                "背得不错，只错了几个字。", "全对了，厉害！"};
    tts_say(SAY[stars]);
    if (stars) {
        static const pet_clip_t EAT[] = {PET_EAT_RICE, PET_EAT_SNACK, PET_EAT_MELON};
        care_feed(stars * 10 + 5);   /* 15 / 25 / 35 */
        play_once(PET_CHEER, EAT[esp_random() % 3]);
    } else {
        play_once(PET_SIGH, PET_IDLE);
    }
    s_busy = false;
}

static void on_heard(const char *text, bool final)
{
    if (!final) return;
    ui_lock();
    if (s_scr) {
        if (!text) {
            set_status("没连上语音识别，再试一次", C_MISS);
            pet_play(s_whale, PET_IDLE, true);
            s_busy = false;
        } else {
            finish(text);
        }
    }
    ui_unlock();
}

/* PWR: hold and recite. */
static void recite_talk(bool down)
{
    if (!s_scr) return;
    if (down) {
        if (s_listening || s_busy) return;
        if (!net_up()) {
            set_status("没有连上网络", C_MISS);
            return;
        }
        tts_stop();
        s_scored = false;
        s_hint = 0;
        render();
        if (!asr_begin(on_heard)) {
            set_status("稍等一下再背", C_DIM);
            return;
        }
        s_listening = true;
        set_status("正在听…背完松开", C_GREEN);
        pet_play(s_whale, PET_LISTEN, true);
    } else if (s_listening) {
        s_listening = false;
        s_busy = true;
        asr_end();
        set_status("小鲸在对答案…", C_DIM);
        pet_play(s_whale, PET_THINK, true);
    }
}

/* ---- buttons, building ---- */

static void build(void);

static void on_hint(lv_event_t *e)
{
    if (s_listening || s_busy) return;
    s_scored = false;
    s_hint = s_hint < 2 ? s_hint + 1 : 0;
    render();
    animate_rows_cascade();
}

static void on_next(lv_event_t *e)
{
    if (s_listening || s_busy) return;
    tts_stop();
    s_poem = (int)(esp_random() % poems_count());
    build();
}

static lv_obj_t *pill(const char *text, uint32_t bg, int x, lv_event_cb_t cb)
{
    lv_obj_t *b = ui_button(s_scr, NULL, &g_font_text, 112, 44, bg, cb, NULL);
    lv_obj_align(b, LV_ALIGN_CENTER, x, 190);
    lv_obj_t *l = ui_label(b, &g_font_text, 0xffffff, text);
    lv_obj_center(l);
    return b;
}

static void build(void)
{
    const poem_t *w = poems_get(s_poem);
    lv_obj_clean(s_scr);
    s_scored = false;
    s_hint = 0;

    s_whale = pet_create(s_scr);
    lv_image_set_scale(s_whale, 128);
    lv_obj_align(s_whale, LV_ALIGN_CENTER, 0, -192);
    pet_play(s_whale, PET_IDLE, true);
    ui_volume_button(s_scr);

    char buf[96];
    snprintf(buf, sizeof(buf), "%s · %s %s", w->title, w->dynasty, w->author);
    lv_obj_t *t = ui_label(s_scr, &g_font_text, C_ACCENT, buf);
    lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(t, 330);
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(t, LV_ALIGN_CENTER, 0, -132);

    s_list = lv_obj_create(s_scr);
    lv_obj_remove_style_all(s_list);
    lv_obj_set_size(s_list, 380, 236);
    lv_obj_align(s_list, LV_ALIGN_CENTER, 0, 10);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_list, 4, 0);
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_OFF);
    for (int i = 0; i < w->nlines; i++) {
        s_rows[i] = lv_spangroup_create(s_list);
        lv_obj_set_width(s_rows[i], LV_SIZE_CONTENT);
        lv_obj_set_style_text_font(s_rows[i], &g_font_poem, 0);
    }
    render();
    animate_rows_cascade();

    s_status = ui_label(s_scr, &g_font_text, C_DIM, "按住 PWR，把这首诗背给小鲸听");
    lv_obj_align(s_status, LV_ALIGN_CENTER, 0, 146);
    pill("提示", C_PANEL, -62, on_hint);
    pill("换一首", 0xd9823c, 62, on_next);
}

static void recite_timer(lv_timer_t *t)
{
    if (!s_scr || !s_whale) return;
    if (pet_finished(s_whale)) {
        pet_clip_t next = s_after;
        s_after = PET_IDLE;
        pet_play(s_whale, next, next == PET_IDLE);
    }
}

static void on_delete(lv_event_t *e)
{
    if (lv_event_get_target(e) != s_scr) return;
    tts_stop();
    if (s_listening || s_busy) asr_cancel();
    s_listening = s_busy = false;
    s_scr = s_whale = s_status = s_list = NULL;
}

static void recite_back(void) { ui_home(); }

void ui_recite(void)
{
    s_poem = ui_poem_current();
    if (s_poem < 0) s_poem = (int)(esp_random() % poems_count());
    s_scr = ui_screen();
    lv_obj_add_event_cb(s_scr, on_delete, LV_EVENT_DELETE, NULL);
    s_after = PET_IDLE;
    build();
    static lv_timer_t *timer;
    if (!timer) timer = lv_timer_create(recite_timer, 150, NULL);
    ui_show(s_scr, recite_back, recite_talk);
}

void ui_recite_inject(const char *heard)   /* as if recited (testing over USB) */
{
    if (s_scr) finish(heard);
}
