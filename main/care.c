#include "care.h"

#include <time.h>

#include "esp_log.h"
#include "net.h"
#include "settings.h"

static const char *TAG = "care";
#define DECAY_PER_HOUR 4   /* full to empty in about a day */

typedef struct {
    uint8_t food;
    uint8_t pad;
    uint16_t day;     /* the day `days` bit 0 is (days since 2026-01-01) */
    uint32_t days;    /* bit k: a check-in k days before `day` */
    uint32_t fed_at;  /* time() of the last update to food */
} care_t;

static care_t s_c = {.food = 70};

static int today(void)
{
    if (!net_time_valid()) return -1;
    return (int)((time(NULL) + 8 * 3600 - 1767225600) / 86400);
}

/* Slide the check-in bits so bit 0 is today. */
static void roll(void)
{
    int d = today();
    if (d < 0) return;
    if (d != s_c.day) {
        int shift = d - s_c.day;
        s_c.days = shift > 0 && shift < 32 ? s_c.days << shift : shift > 0 ? 0 : s_c.days;
        s_c.day = (uint16_t)d;
    }
}

static void decay(void)
{
    if (!net_time_valid()) return;
    uint32_t now = (uint32_t)time(NULL);
    if (s_c.fed_at == 0) s_c.fed_at = now;
    int hours = (int)((now - s_c.fed_at) / 3600);
    if (hours > 0) {
        int f = s_c.food - hours * DECAY_PER_HOUR;
        s_c.food = (uint8_t)(f < 0 ? 0 : f);
        s_c.fed_at += hours * 3600;
    }
}

static void save(void) { settings_set_blob("care", &s_c, sizeof(s_c)); }

void care_init(void)
{
    care_t c;
    if (settings_get_blob("care", &c, sizeof(c))) s_c = c;
    ESP_LOGI(TAG, "food %d, check-ins %08lx", s_c.food, (unsigned long)s_c.days);
}

int care_food(void)
{
    decay();
    return s_c.food;
}

void care_feed(int amount)
{
    decay();
    roll();
    int f = s_c.food + amount;
    s_c.food = (uint8_t)(f > 100 ? 100 : f);
    if (today() >= 0) s_c.days |= 1;
    if (net_time_valid()) s_c.fed_at = (uint32_t)time(NULL);
    save();
    ESP_LOGI(TAG, "fed %d -> %d", amount, s_c.food);
}

uint8_t care_week(void)
{
    roll();
    return (uint8_t)(s_c.days & 0x7F);
}

int care_streak(void)
{
    roll();
    uint32_t d = s_c.days;
    int n = 0, start = d & 1 ? 0 : 1;   /* today not done yet still counts yesterday's run */
    for (int k = start; k < 32 && (d >> k & 1); k++) n++;
    return n;
}
