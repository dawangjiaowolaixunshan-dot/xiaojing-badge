#pragma once
#include <stdbool.h>

/*
 * The AXP2101 power chip. PWR is also the talk key on the 1.75C, and the chip
 * powers off when PWR is held past its hold-off: set to the longest (10 s)
 * at boot, and off altogether while a talk holds the key (as Muse does).
 */
void pmu_init(void);
void pmu_key_shutdown(bool on);
bool pmu_read(int *pct, bool *charging, bool *usb);   /* pct -1 without a battery */
void pmu_power_off(void);
