#include "poems.h"

#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "poems";
extern const char poems_txt_start[] asm("_binary_poems_txt_start");

#define MAX_POEMS 512
static poem_t *s_poems;
static int s_n;

static char *next_field(char **p, char sep)
{
    char *start = *p, *e = strchr(start, sep);
    if (e) {
        *e = '\0';
        *p = e + 1;
    } else {
        *p = start + strlen(start);
    }
    return start;
}

void poems_init(void)
{
    size_t len = strlen(poems_txt_start);
    char *text = heap_caps_malloc(len + 1, MALLOC_CAP_SPIRAM);
    s_poems = heap_caps_calloc(MAX_POEMS, sizeof(poem_t), MALLOC_CAP_SPIRAM);
    assert(text && s_poems);
    memcpy(text, poems_txt_start, len + 1);
    char *p = text;
    while (*p && s_n < MAX_POEMS) {
        char *row = next_field(&p, '\n');
        poem_t *w = &s_poems[s_n];
        w->id = next_field(&row, '\t');
        w->title = next_field(&row, '\t');
        w->author = next_field(&row, '\t');
        w->dynasty = next_field(&row, '\t');
        w->level = (uint8_t)atoi(next_field(&row, '\t'));
        w->theme = next_field(&row, '\t');
        while (*row && w->nlines < POEM_MAX_LINES) {
            w->lines[w->nlines++] = next_field(&row, '|');
        }
        if (w->nlines) s_n++;
    }
    ESP_LOGI(TAG, "%d poems", s_n);
}

int poems_count(void) { return s_n; }
const poem_t *poems_get(int i) { return i >= 0 && i < s_n ? &s_poems[i] : NULL; }
