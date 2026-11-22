#include "prefs.h"

#include "settings.h"
#include "tts.h"
#include "ui.h"

const int PREFS_SLEEP_S[PREFS_SLEEP_N] = {0, 30, 60, 180};
const char *const PREFS_SLEEP_NAMES[PREFS_SLEEP_N] = {"不熄屏", "30 秒", "1 分钟", "3 分钟"};

/*
 * High-definition 24 kHz voices:
 * - "q3:<Voice>" -> qwen3-tts-flash-realtime (high-clarity 24 kHz expressive model)
 * - plain name   -> cosyvoice-v2 at 24 kHz (also supports sentence-begin sync for poems)
 */
const char *const PREFS_VOICES[PREFS_VOICE_N] = {
    "q3:Cherry",
    "q3:Maia",
    "q3:Serena",
    "q3:Mia",
    "q3:Chelsie",
    "longanrou",
    "longxiaochun_v2",
    "longanya",
};

const char *const PREFS_VOICE_NAMES[PREFS_VOICE_N] = {
    "芊悦 · 阳光自然",
    "四月 · 干净知性",
    "苏瑶 · 温柔舒服",
    "米娅 · 轻柔细腻",
    "千雪 · 甜美活泼",
    "龙安柔 · 温柔姐姐",
    "龙小淳 · 知性自然",
    "龙安雅 · 古风清亮",
};

prefs_t g_prefs = {.brightness = 60, .volume = 75, .sleep = 1, .voice = 0, .poem_voice = 1};

void prefs_load(void)
{
    prefs_t p;
    if (settings_get_blob("prefs", &p, sizeof(p)) && p.brightness >= 10 && p.brightness <= 100 &&
        p.volume <= 100 && p.sleep < PREFS_SLEEP_N) {
        if (p.voice >= PREFS_VOICE_N) p.voice = 0;
        if (p.poem_voice >= PREFS_VOICE_N) p.poem_voice = 1;
        g_prefs = p;
    }
}

void prefs_save(void) { settings_set_blob("prefs", &g_prefs, sizeof(g_prefs)); }

void prefs_apply(void)
{
    ui_set_brightness(g_prefs.brightness);
    tts_set_volume(g_prefs.volume);
    tts_set_voice(PREFS_VOICES[g_prefs.voice]);
}
