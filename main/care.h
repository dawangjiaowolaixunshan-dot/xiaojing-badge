#pragma once
#include <stdbool.h>
#include <stdint.h>

/*
 * Looking after 小鲸: she gets hungry over a day, and learning feeds her
 * (a poem recited, a poem read to the end). Each day with some learning is a
 * check-in; the home screen shows the last seven.
 */
void care_init(void);
int care_food(void);              /* 0..100, as of now */
void care_feed(int amount);       /* and checks today in */
uint8_t care_week(void);          /* bit 0 today, bit 6 six days ago */
int care_streak(void);            /* days in a row up to today (or yesterday) */
