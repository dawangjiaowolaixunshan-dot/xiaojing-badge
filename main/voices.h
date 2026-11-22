#pragma once
#include <stdbool.h>

#include "vp.h"

/*
 * Who's talking: the child (小鲸's owner), dad or mum, each recorded in 设置
 * with a few sentences. Matched by pitch and timbre (MFCC); not a forensic
 * voiceprint, but enough to tell a family of three apart.
 */
typedef enum { VOICE_CHILD, VOICE_DAD, VOICE_MOM, VOICE_COUNT, VOICE_UNKNOWN = -1 } voice_who_t;

#define VOICE_ENROLL_SENTENCES 3

void voices_init(void);
bool voices_enabled(void);
void voices_set_enabled(bool on);
int voices_enrolled_hz(voice_who_t who);        /* 0 when not recorded */
int voices_enrolled_n(voice_who_t who);         /* sentences recorded with timbre (0: none, or pitch only) */
void voices_reset(voice_who_t who);
void voices_reset_all(void);
void voices_enroll(voice_who_t who, const voice_sample_t *s);
/* The best match, or VOICE_UNKNOWN. Logged to /hist/voice.csv for tuning. */
voice_who_t voices_classify(const voice_sample_t *s);
float voices_score(voice_who_t who, const voice_sample_t *s);   /* how far s is from who (lower is closer) */
const char *voices_name(voice_who_t who);       /* 孩子 / 爸爸 / 妈妈; "" when unknown */
const char *voices_key(voice_who_t who);        /* child / dad / mom, for the history */
voice_who_t voices_from_key(const char *key);
