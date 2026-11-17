#pragma once
#include <stdbool.h>
#include <stdint.h>

#define POEM_MAX_LINES 18
#define POEM_LEVELS 4

typedef struct {
    const char *id, *title, *author, *dynasty, *theme;
    uint8_t level, nlines;
    const char *lines[POEM_MAX_LINES];
} poem_t;

void poems_init(void);
int poems_count(void);
const poem_t *poems_get(int i);
