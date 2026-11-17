#pragma once
#include <stdbool.h>

/*
 * Battery life: a log of level and voltage every minute (/hist/power.csv,
 * exported over USB), and deep sleep after the screen has been dark a while
 * on battery. PWR or BOOT wakes it (a fresh boot, about 3 s).
 */
void power_start(void);
void power_dump(void (*line_fn)(const char *line));
void power_deep_sleep(int timer_s);   /* doesn't return; timer_s > 0 also wakes after that long */
void power_log_wake(void);           /* at boot: what woke it, if deep sleep */
