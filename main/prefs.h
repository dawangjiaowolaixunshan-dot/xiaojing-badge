#pragma once
#include <stdint.h>

/* What the settings page changes, kept in NVS ("prefs"). */
typedef struct {
    uint8_t brightness;   /* 10..100 */
    uint8_t volume;       /* 0..100 */
    uint8_t sleep;        /* index into PREFS_SLEEP_S */
    uint8_t voice;        /* index into PREFS_VOICES: 小鲸's chat */
    uint8_t poem_voice;   /* index into PREFS_VOICES: reading poems */
} prefs_t;

#define PREFS_SLEEP_N 4
extern const int PREFS_SLEEP_S[PREFS_SLEEP_N];          /* 0 = never */
extern const char *const PREFS_SLEEP_NAMES[PREFS_SLEEP_N];
#define PREFS_VOICE_N 13
extern const char *const PREFS_VOICES[PREFS_VOICE_N];
extern const char *const PREFS_VOICE_NAMES[PREFS_VOICE_N];

extern prefs_t g_prefs;
void prefs_load(void);
void prefs_save(void);
void prefs_apply(void);   /* brightness, volume, voice */
