#include "words.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "net.h"
#include "settings.h"

static const char *TAG = "words";
extern const char words_txt_start[] asm("_binary_words_txt_start");
#define MAX_WORDS 400

typedef struct __attribute__((packed)) {
    uint8_t box;    /* 0 new; each right answer one box up */
    uint16_t due;   /* day number it's due again */
} prog_t;

static word_t *s_w;
static prog_t *s_p;
static int s_n;

static int today(void)
{
    if (!net_time_valid()) return 0;
    return (int)((time(NULL) + 8 * 3600 - 1767225600) / 86400);
}

void words_init(void)
{
    size_t len = strlen(words_txt_start);
    char *text = heap_caps_malloc(len + 1, MALLOC_CAP_SPIRAM);
    s_w = heap_caps_calloc(MAX_WORDS, sizeof(word_t), MALLOC_CAP_SPIRAM);
    s_p = heap_caps_calloc(MAX_WORDS, sizeof(prog_t), MALLOC_CAP_SPIRAM);
    if (!text || !s_w || !s_p) return;
    memcpy(text, words_txt_start, len + 1);
    for (char *line = strtok(text, "\n"); line && s_n < MAX_WORDS; line = strtok(NULL, "\n")) {
        char *g = line, *w = strchr(g, '\t'), *m = w ? strchr(w + 1, '\t') : NULL;
        if (!w || !m) continue;
        *w++ = '\0';
        *m++ = '\0';
        s_w[s_n++] = (word_t){.word = w, .meaning = m, .grade = (uint8_t)atoi(g)};
    }
    if (!settings_get_blob("words", s_p, s_n * sizeof(prog_t))) memset(s_p, 0, MAX_WORDS * sizeof(prog_t));
    ESP_LOGI(TAG, "%d words, %d known", s_n, words_known());
}

int words_count(void) { return s_n; }
const word_t *words_get(int i) { return i >= 0 && i < s_n ? &s_w[i] : NULL; }

int words_today(int *out, int cap)
{
    int d = today(), k = 0;
    if (cap > WORDS_PER_DAY) cap = WORDS_PER_DAY;
    for (int i = 0; i < s_n && k < cap; i++)
        if (s_p[i].box > 0 && s_p[i].due <= d) out[k++] = i;   /* reviews due */
    for (int i = 0; i < s_n && k < cap; i++)
        if (s_p[i].box == 0) out[k++] = i;                      /* then new, in grade order */
    return k;
}

void words_mark(int i, bool right)
{
    static const uint8_t GAP[] = {0, 1, 2, 4, 7, 15, 30, 60};
    if (i < 0 || i >= s_n) return;
    prog_t *p = &s_p[i];
    if (right) {
        p->box = p->box < 7 ? p->box + 1 : 7;
        p->due = (uint16_t)(today() + GAP[p->box]);
    } else {
        p->box = 1;
        p->due = (uint16_t)(today() + 1);
    }
    settings_set_blob("words", s_p, s_n * sizeof(prog_t));
}

int words_known(void)
{
    int n = 0;
    for (int i = 0; i < s_n; i++) n += s_p[i].box >= 2;
    return n;
}

/* Letters only, lower case. */
static void norm(const char *in, char *out, size_t cap)
{
    size_t o = 0;
    for (; *in && o + 1 < cap; in++)
        if (isalpha((unsigned char)*in)) out[o++] = (char)tolower((unsigned char)*in);
    out[o] = '\0';
}

static int edit_distance(const char *a, const char *b)
{
    int la = strlen(a), lb = strlen(b);
    if (la > 31 || lb > 31) return 99;
    int d[32][32];
    for (int i = 0; i <= la; i++) d[i][0] = i;
    for (int j = 0; j <= lb; j++) d[0][j] = j;
    for (int i = 1; i <= la; i++)
        for (int j = 1; j <= lb; j++) {
            int c = d[i - 1][j - 1] + (a[i - 1] != b[j - 1]);
            int x = d[i - 1][j] + 1, y = d[i][j - 1] + 1;
            d[i][j] = c < x ? (c < y ? c : y) : (x < y ? x : y);
        }
    return d[la][lb];
}

bool words_match(const char *word, const char *heard)
{
    char w[48], h[160];
    norm(word, w, sizeof(w));
    norm(heard, h, sizeof(h));
    if (!w[0] || !h[0]) return false;
    if (strstr(h, w)) return true;   /* "apple", "an apple", "Apple." */
    /* the recogniser's spelling slips on longer words */
    return strlen(w) >= 5 && edit_distance(w, h) <= 1;
}
