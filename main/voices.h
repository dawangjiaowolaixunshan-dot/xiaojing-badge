#pragma once
#include <stdbool.h>

/*
 * Who's talking, from the pitch of their voice: the child (小鲸's owner),
 * dad or mum, each recorded once in 设置. A ten-year-old's voice sits around
 * twice a man's, so this tells them apart; it is not a voiceprint.
 */
typedef enum { VOICE_CHILD, VOICE_DAD, VOICE_MOM, VOICE_COUNT, VOICE_UNKNOWN = -1 } voice_who_t;

void voices_init(void);
int voices_enrolled_hz(voice_who_t who);        /* 0 when not recorded */
void voices_enroll(voice_who_t who, int hz);
voice_who_t voices_classify(int hz);
const char *voices_name(voice_who_t who);       /* 孩子 / 爸爸 / 妈妈; "" when unknown */
const char *voices_key(voice_who_t who);        /* child / dad / mom, for the history */
