/* Host test: cc -I main tests/math/test_math.c main/mathq.c main/recite.c main/pinyin_table.c */
#include <stdio.h>
#include <stdlib.h>

#include "mathq.h"

static int fails;
static void check(const char *s, int want)
{
    int got = math_parse_answer(s);
    printf("%-22s -> %d%s\n", s, got, got == want ? "" : "   WRONG");
    if (got != want) fails++;
}
static unsigned rnd(void) { return (unsigned)rand(); }

int main(void)
{
    check("85", 85); check("答案是85。", 85); check("八十五", 85); check("三十七", 37);
    check("十二", 12); check("十", 10); check("一百零五", 105); check("两百", 200);
    check("九百九十九", 999); check("一千零二十", 1020); check("七", 7); check("零", 0);
    check("等于四十二", 42); check("嗯", -1);
    for (int lv = 0; lv < MATH_LEVELS; lv++) {
        for (int i = 0; i < 2000; i++) {
            math_q_t q;
            math_make(lv, &q, rnd);
            if (q.answer < 0 || q.answer > 1000) { printf("level %d bad: %s %d\n", lv, q.text, q.answer); fails++; break; }
        }
        math_q_t q; math_make(lv, &q, rnd); printf("level %d (%s): %s %d\n", lv, math_level_name(lv), q.text, q.answer);
    }
    printf(fails ? "FAILED %d\n" : "all passed\n", fails);
    return fails != 0;
}
