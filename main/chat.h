#pragma once
#include <stdbool.h>

/*
 * DeepSeek chat (deepseek-chat), streamed. The reply grows through on_text;
 * on_done gets NULL on success or a short error for the screen. Keeps the
 * last few exchanges as context until chat_reset().
 */
typedef void (*chat_text_cb_t)(const char *reply);
typedef void (*chat_done_cb_t)(const char *error);

bool chat_ask(const char *question, chat_text_cb_t on_text, chat_done_cb_t on_done);
void chat_reset(void);     /* a new conversation, marked in the history */
void chat_restore(void);   /* context since the last new conversation, at boot */
bool chat_busy(void);
