#pragma once
#include <stdbool.h>

/*
 * Mental arithmetic for a primary schooler: six levels, ten questions a
 * round; a round with 9+ right goes up a level, 5 or fewer goes down.
 */
#define MATH_LEVELS 6

typedef struct {
    char text[32];   /* "37 + 48 =" */
    int answer;
} math_q_t;

void math_make(int level, math_q_t *q, unsigned (*rnd)(void));
const char *math_level_name(int level);
/* The number in what was said: digits ("85") or Chinese ("八十五", "一百零五", "两百"); -1 if none. */
int math_parse_answer(const char *heard);
