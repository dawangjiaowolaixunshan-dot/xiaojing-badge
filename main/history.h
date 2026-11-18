#pragma once
#include <stdbool.h>
#include <stddef.h>

/*
 * The DeepSeek conversation on flash (SPIFFS "storage", /hist/chat.jsonl):
 * one JSON line per exchange, {"q":..,"a":..}, or {"new":1} where a new
 * conversation began. Kept to roughly the last MAX entries.
 */
#define HISTORY_SHOW 30

void history_init(void);
void history_add(const char *q, const char *a, const char *who);   /* who: child/dad/mom or NULL */
void history_divider(void);
/* The last `max` entries, oldest first; q == NULL marks a divider. */
void history_each(int max, void (*fn)(const char *q, const char *a, void *ctx), void *ctx);
const char *history_each_who(void);   /* inside history_each's fn: who asked (child/dad/mom, or "") */

/* Every saved line, oldest first, as stored (for the export over USB). */
void history_dump(void (*line_fn)(const char *line));

/* The custom system prompt (/hist/prompt.md); NULL when there is none. */
#define PROMPT_MAX (16 * 1024)
char *prompt_load(void);                    /* free() it */
bool prompt_save(const char *text, size_t len);   /* len 0 removes it */

/* Recall: the record kept in PSRAM too, and searched by a question's words
 * for past exchanges about the same thing (beyond the newest skip_newest,
 * which are in the context already). Writes "问：… 答：…" lines. */
void history_recall_init(void);
void history_recall_add(const char *q, const char *a);
size_t history_recall(const char *question, int skip_newest, char *out, size_t cap);
