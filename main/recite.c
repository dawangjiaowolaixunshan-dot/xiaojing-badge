#include "recite.h"

#include <stdlib.h>
#include <string.h>

#include "pinyin_table.h"

int recite_utf8_next(const char **pp)
{
    const unsigned char *p = (const unsigned char *)*pp;
    if (!*p) return 0;
    int n = *p < 0x80 ? 1 : *p < 0xE0 ? 2 : *p < 0xF0 ? 3 : 4;
    int cp = n == 1 ? *p : *p & (0x7F >> n);
    for (int i = 1; i < n; i++) {
        if (!p[i]) {
            *pp = (const char *)p + i;
            return 0;
        }
        cp = cp << 6 | (p[i] & 0x3F);
    }
    *pp = (const char *)p + n;
    return cp;
}

bool recite_is_hanzi(int cp) { return cp >= 0x4E00 && cp <= 0x9FFF; }

static const pinyin_row_t *row_of(uint32_t cp)
{
    int lo = 0, hi = PINYIN_NROWS - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (PINYIN_ROWS[mid].cp == cp) return &PINYIN_ROWS[mid];
        if (PINYIN_ROWS[mid].cp < cp) lo = mid + 1;
        else hi = mid - 1;
    }
    return NULL;
}

bool pinyin_sounds_alike(uint32_t a, uint32_t b)
{
    if (a == b) return true;
    const pinyin_row_t *ra = row_of(a), *rb = row_of(b);
    if (!ra || !rb) return false;
    for (int i = 0; i < 3 && ra->py[i]; i++)
        for (int j = 0; j < 3 && rb->py[j]; j++)
            if (ra->py[i] == rb->py[j]) return true;
    return false;
}

static int hanzi_of(const char *s, int *out, int cap)
{
    int n = 0, cp;
    while ((cp = recite_utf8_next(&s)) != 0 && n < cap) {
        if (recite_is_hanzi(cp)) out[n++] = cp;
    }
    return n;
}

bool recite_score(const char *poem, const char *heard, recite_result_t *out)
{
    /* Off the stack and out of internal RAM: malloc puts these in PSRAM. */
    int *P = malloc(RECITE_MAX * sizeof(int)), *H = malloc(2 * RECITE_MAX * sizeof(int));
    memset(out, 0, sizeof(*out));
    if (!P || !H) {
        free(P);
        free(H);
        return false;
    }
    int n = hanzi_of(poem, P, RECITE_MAX), m = hanzi_of(heard, H, 2 * RECITE_MAX);
    out->n = n;
    uint16_t *t = n ? calloc((size_t)(n + 1) * (m + 1), sizeof(uint16_t)) : NULL;   /* the LCS table */
    if (!t) {
        free(P);
        free(H);
        return false;
    }
#define T(i, j) t[(size_t)(i) * (m + 1) + (j)]
    for (int i = 1; i <= n; i++)
        for (int j = 1; j <= m; j++)
            T(i, j) = pinyin_sounds_alike(P[i - 1], H[j - 1]) ? T(i - 1, j - 1) + 1
                      : T(i - 1, j) >= T(i, j - 1)            ? T(i - 1, j)
                                                              : T(i, j - 1);
    for (int i = n, j = m; i > 0 && j > 0;) {
        if (pinyin_sounds_alike(P[i - 1], H[j - 1]) && T(i, j) == T(i - 1, j - 1) + 1) {
            out->ok[i - 1] = 1;
            i--;
            j--;
        } else if (T(i - 1, j) >= T(i, j - 1)) {
            i--;
        } else {
            j--;
        }
    }
#undef T
    free(t);
    free(P);
    free(H);
    for (int i = 0; i < n; i++) out->right += out->ok[i];
    return true;
}
