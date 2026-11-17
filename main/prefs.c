#include "prefs.h"

#include "bsp/display.h"
#include "settings.h"
#include "tts.h"

const int PREFS_SLEEP_S[PREFS_SLEEP_N] = {0, 30, 60, 180};
const char *const PREFS_SLEEP_NAMES[PREFS_SLEEP_N] = {"不熄屏", "30 秒", "1 分钟", "3 分钟"};
/* cosyvoice-v2 voices (Bailian's list), picked for a ten-year-old. */
const char *const PREFS_VOICES[PREFS_VOICE_N] = {
    "longhua_v2", "longxiaochun_v2", "longjielidou_v2", "longniuniu", "longhuhu",
    "longpaopao", "longhouge", "longjiqi", "libai_v2", "longfei_v2",
};
const char *const PREFS_VOICE_NAMES[PREFS_VOICE_N] = {
    "龙华 · 元气女声", "龙小淳 · 知性女声", "龙杰力豆 · 顽皮男孩", "龙牛牛 · 阳光男童", "龙呼呼 · 可爱女童",
    "龙泡泡 · 泡泡音", "龙猴哥 · 猴哥", "龙机器 · 呆萌机器人", "李白 · 诗仙", "龙飞 · 朗诵男声",
};

prefs_t g_prefs = {.brightness = 80, .volume = 75, .sleep = 2, .voice = 0, .poem_voice = 8};

void prefs_load(void)
{
    prefs_t p;
    if (settings_get_blob("prefs", &p, sizeof(p)) && p.brightness >= 10 && p.brightness <= 100 &&
        p.volume <= 100 && p.sleep < PREFS_SLEEP_N && p.voice < PREFS_VOICE_N && p.poem_voice < PREFS_VOICE_N) {
        g_prefs = p;
    }
}

void prefs_save(void) { settings_set_blob("prefs", &g_prefs, sizeof(g_prefs)); }

void prefs_apply(void)
{
    bsp_display_brightness_set(g_prefs.brightness);
    tts_set_volume(g_prefs.volume);
    tts_set_voice(PREFS_VOICES[g_prefs.voice]);
}
