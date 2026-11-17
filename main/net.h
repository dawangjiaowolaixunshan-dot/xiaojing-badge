#pragma once
#include <stdbool.h>

void net_start(void);
bool net_wait(int timeout_ms);    /* true once the STA has an IP */
bool net_up(void);
bool net_time_valid(void);        /* SNTP has set the clock */
void net_reconnect(void);
int net_rssi(void);                /* dBm of the joined AP; -127 when not joined */         /* after the Wi-Fi settings change */
