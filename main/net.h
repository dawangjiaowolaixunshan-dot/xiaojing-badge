#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    char ssid[33];
    int8_t rssi;
    bool secured;       /* authmode != WIFI_AUTH_OPEN */
    int8_t saved_slot;  /* >=0 if already saved in settings, else -1 */
} net_ap_t;

typedef enum {
    NET_STATE_IDLE,
    NET_STATE_SCANNING,
    NET_STATE_CONNECTING,
    NET_STATE_CONNECTED,
    NET_STATE_FAILED,
} net_state_t;

void net_start(void);
bool net_wait(int timeout_ms);    /* true once the STA has an IP; <0 or portMAX_DELAY waits forever */
bool net_up(void);
bool net_time_valid(void);        /* SNTP has set the clock */
void net_reconnect(void);         /* after Wi-Fi settings change: scan and join the best known */
int net_slot(void);               /* the saved-network slot in use (-1 none) */
void net_join(int slot);          /* join this saved network immediately (even before scan) */
void net_hold(bool hold);         /* provisioning has the radio: don't join */
int net_rssi(void);               /* dBm of the joined AP; -127 when not joined */
void net_awake(bool on);          /* hold Wi-Fi out of modem sleep (nests) */
void net_suspend(bool off);       /* Wi-Fi off while the screen is dark, back on with it */

/* Pure-STA scan of nearby 2.4 GHz networks (deduplicated, strongest first). */
int net_scan(net_ap_t *out, int cap);
net_state_t net_state(void);
int net_last_reason(void);
void net_target_ssid(char *out, size_t cap);
const char *net_status_text(char *buf, size_t cap);
