#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Scoring a recited poem: the recognised text against the poem, both cut to
 * their Chinese characters, aligned (longest common subsequence) with a
 * character counting as right when it sounds the same (the recogniser often
 * writes a homophone). Punctuation is neither right nor wrong.
 */
#define RECITE_MAX 400   /* characters of the poem */

typedef struct {
    int n;                       /* the poem's characters (punctuation excluded) */
    int right;
    uint8_t ok[RECITE_MAX];      /* per poem character: 1 recited, 0 missed */
} recite_result_t;

/* poem: the lines joined; heard: what the recogniser wrote. */
bool recite_score(const char *poem, const char *heard, recite_result_t *out);
int recite_utf8_next(const char **p);   /* the next code point, advancing *p; 0 at the end */
bool recite_is_hanzi(int cp);
