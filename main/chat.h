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
void chat_set_speaker(int who);   /* voice_who_t of the next question (voices.h) */

/* Called (from the chat task) when DeepSeek starts a web search. */
typedef void (*chat_search_cb_t)(void);
void chat_on_search(chat_search_cb_t cb);
