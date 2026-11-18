#pragma once
#include <stdbool.h>
#include <stdint.h>

/*
 * English words for primary school (data/words.txt: grade, word, meaning),
 * learned ten a day: the ones due for review first, then new ones in grade
 * order. Read right, a word comes back after longer and longer gaps; wrong,
 * tomorrow.
 */
typedef struct {
    const char *word, *meaning;
    uint8_t grade;
} word_t;

#define WORDS_PER_DAY 10

void words_init(void);
int words_count(void);
const word_t *words_get(int i);
int words_today(int *out, int cap);     /* today's set: indices */
void words_mark(int i, bool right);
int words_known(void);                  /* read right at least twice */
bool words_match(const char *word, const char *heard);   /* what was heard is the word */
