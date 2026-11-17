#pragma once
#include <stdbool.h>

void net_start(void);
bool net_wait(int timeout_ms);    /* true once the STA has an IP */
bool net_up(void);
bool net_time_valid(void);        /* SNTP has set the clock */
void net_reconnect(void);          /* after the Wi-Fi settings change; starts from home Wi-Fi */
int net_slot(void);                /* Wi-Fi slot in use: 0 home, 1 phone hotspot */
int net_rssi(void);                /* dBm of the joined AP; -127 when not joined */
void net_awake(bool on);           /* hold Wi-Fi out of modem sleep (nests) */
void net_suspend(bool off);        /* Wi-Fi off while the screen is dark, back on with it */
