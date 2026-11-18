#pragma once
#include <stdbool.h>

/*
 * Streaming speech recognition with Alibaba Cloud Bailian (DashScope)
 * paraformer-realtime-v2 over its WebSocket API. Audio starts buffering at
 * asr_begin(), before the socket is up, so nothing said early is lost.
 */
typedef void (*asr_text_cb_t)(const char *text, bool final);

bool asr_begin(asr_text_cb_t cb);   /* false: no key */
bool asr_begin_lang(asr_text_cb_t cb, const char *langs);   /* langs: e.g. "\"en\"" (JSON array body) */
void asr_end(void);                 /* stop listening; the final text follows */
void asr_cancel(void);
