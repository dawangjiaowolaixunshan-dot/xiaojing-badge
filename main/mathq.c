#include "mathq.h"

#include <stdio.h>
#include <string.h>

#include "recite.h"   /* recite_utf8_next */

const char *math_level_name(int level)
{
    static const char *N[MATH_LEVELS] = {"20 以内加减", "100 以内加减", "乘法口诀", "两位数乘一位数", "整除除法", "混合运算"};
    return level >= 0 && level < MATH_LEVELS ? N[level] : "";
}

static int r(unsigned (*rnd)(void), int lo, int hi) { return lo + (int)(rnd() % (unsigned)(hi - lo + 1)); }

void math_make(int level, math_q_t *q, unsigned (*rnd)(void))
{
    int a, b, c;
    switch (level) {
    case 0:   /* within 20 */
        a = r(rnd, 1, 19);
        if (rnd() % 2) {
            b = r(rnd, 1, 20 - a);
            snprintf(q->text, sizeof(q->text), "%d + %d =", a, b);
            q->answer = a + b;
        } else {
            b = r(rnd, 1, a);
            snprintf(q->text, sizeof(q->text), "%d - %d =", a, b);
            q->answer = a - b;
        }
        break;
    case 1:   /* within 100 */
        a = r(rnd, 11, 99);
        if (rnd() % 2) {
            b = r(rnd, 5, 100 - a > 5 ? 100 - a : 5);
            if (a + b > 100) b = 100 - a;
            snprintf(q->text, sizeof(q->text), "%d + %d =", a, b);
            q->answer = a + b;
        } else {
            b = r(rnd, 5, a);
            snprintf(q->text, sizeof(q->text), "%d - %d =", a, b);
            q->answer = a - b;
        }
        break;
    case 2:   /* the times tables, and their divisions */
        a = r(rnd, 2, 9);
        b = r(rnd, 2, 9);
        if (rnd() % 3) {
            snprintf(q->text, sizeof(q->text), "%d × %d =", a, b);
            q->answer = a * b;
        } else {
            snprintf(q->text, sizeof(q->text), "%d ÷ %d =", a * b, a);
            q->answer = b;
        }
        break;
    case 3:   /* two digits by one */
        a = r(rnd, 11, 49);
        b = r(rnd, 2, 9);
        snprintf(q->text, sizeof(q->text), "%d × %d =", a, b);
        q->answer = a * b;
        break;
    case 4:   /* exact division, up to three digits by one */
        b = r(rnd, 2, 9);
        c = r(rnd, 11, 99);
        snprintf(q->text, sizeof(q->text), "%d ÷ %d =", b * c, b);
        q->answer = c;
        break;
    default:   /* mixed: a + b × c, or (a - b) × c */
        a = r(rnd, 2, 30);
        b = r(rnd, 2, 9);
        c = r(rnd, 2, 9);
        if (rnd() % 2) {
            snprintf(q->text, sizeof(q->text), "%d + %d × %d =", a, b, c);
            q->answer = a + b * c;
        } else {
            a = r(rnd, b + 1, b + 12);
            snprintf(q->text, sizeof(q->text), "(%d - %d) × %d =", a, b, c);
            q->answer = (a - b) * c;
        }
        break;
    }
}

static int digit_of(int cp)
{
    static const int CP[] = {0x96F6, 0x4E00, 0x4E8C, 0x4E09, 0x56DB, 0x4E94, 0x516D, 0x4E03, 0x516B, 0x4E5D};   /* 零一二三四五六七八九 */
    for (int i = 0; i < 10; i++)
        if (cp == CP[i]) return i;
    if (cp == 0x4E24) return 2;   /* 两 */
    if (cp == 0x3007) return 0;   /* 〇 */
    return -1;
}

int math_parse_answer(const char *heard)
{
    /* Digits first: the recogniser often writes them. */
    for (const char *p = heard; *p; p++) {
        if (*p >= '0' && *p <= '9') {
            int v = 0;
            while (*p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0');
            return v;
        }
    }
    /* Chinese numerals, up to 9999. */
    int total = 0, cur = -1, cp;
    bool any = false;
    const char *p = heard;
    while ((cp = recite_utf8_next(&p)) != 0) {
        int d = digit_of(cp);
        if (d >= 0) {
            cur = d;
            any = true;
            continue;
        }
        int unit = cp == 0x5341 ? 10 : cp == 0x767E ? 100 : cp == 0x5343 ? 1000 : 0;   /* 十百千 */
        if (unit) {
            total += (cur < 0 ? 1 : cur) * unit;   /* 十二 starts with an implied 一 */
            cur = -1;
            any = true;
        } else if (any) {
            break;   /* the number has ended */
        }
    }
    if (!any) return -1;
    return total + (cur > 0 ? cur : 0);
}
