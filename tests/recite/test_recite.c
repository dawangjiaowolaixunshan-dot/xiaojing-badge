/* Host test: cc -I main tests/recite/test_recite.c main/recite.c main/pinyin_table.c */
#include <stdio.h>
#include <string.h>

#include "recite.h"

static int fails;
static void check(const char *name, const char *poem, const char *heard, int want_right)
{
    recite_result_t r;
    recite_score(poem, heard, &r);
    printf("%-34s %d/%d (want %d)\n", name, r.right, r.n, want_right);
    if (r.right != want_right) fails++;
}

int main(void)
{
    const char *p = "床前明月光，疑是地上霜。举头望明月，低头思故乡。";
    check("perfect", p, "床前明月光疑是地上霜举头望明月低头思故乡", 20);
    check("homophone 疑->仪, 霜->双", p, "床前明月光仪是地上双举头望明月低头思故乡", 20);
    check("missed the third line", p, "床前明月光疑是地上霜低头思故乡", 15);
    check("one wrong character", p, "床前明月光疑是地上霜举头望白月低头思故乡", 19);
    check("extra words around", p, "嗯我背一下床前明月光疑是地上霜举头望明月低头思故乡背完了", 20);
    check("nothing", p, "", 0);
    printf(fails ? "FAILED %d\n" : "all passed\n", fails);
    return fails != 0;
}
