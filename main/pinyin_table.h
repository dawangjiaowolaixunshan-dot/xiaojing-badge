#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Toneless readings of ~7000 characters (tools/make_pinyin.py), for telling
 * a recited character from a homophone the recogniser wrote instead. */
typedef struct {
    uint16_t cp;
    uint16_t py[3];   /* syllable ids; 0 = none */
} pinyin_row_t;

extern const pinyin_row_t PINYIN_ROWS[];
extern const int PINYIN_NROWS;

/* Same character, or sharing a reading. */
bool pinyin_sounds_alike(uint32_t a, uint32_t b);
