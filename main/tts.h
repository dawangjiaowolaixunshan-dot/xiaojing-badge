#pragma once
#include <stdbool.h>

/*
 * Speaking replies with Alibaba Cloud Bailian CosyVoice (cosyvoice-v2) over
 * the DashScope WebSocket API. Text goes in as it arrives (tts_feed), so the
 * first words play while DeepSeek is still writing the rest.
 */
void tts_init(void);
bool tts_begin(void);                 /* false: no key, or still busy */
void tts_feed(const char *text);      /* more of the reply */
void tts_finish(void);                /* no more text; let the audio play out */
void tts_stop(void);                  /* cut off now (a new question) */
bool tts_enabled(void);
bool tts_active(void);                /* still fetching or playing */
int tts_level(void);                  /* loudness of the speech now playing, 0..100 */
void tts_set_enabled(bool on);        /* saved */
void tts_set_voice(const char *voice);   /* a CosyVoice voice id, e.g. longxiaochun_v2 */
void tts_say(const char *text);           /* stop what's playing and speak this, in the chat voice */
void tts_say_with(const char *voice, const char *text);   /* ...in this voice (voice samples, poems); \x1e splits sentences */
void tts_set_volume(int pct);
int tts_sentence_now(void);   /* which sentence of what's playing is being heard (0-based), -1 when none */
