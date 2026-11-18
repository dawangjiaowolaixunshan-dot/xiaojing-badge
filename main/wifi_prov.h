#pragma once
#include <stdbool.h>

/*
 * Adding a Wi-Fi from a phone: the badge opens its own access point
 * (XiaoJing-NNNN, a random 8-digit password, WPA2), answers every DNS query
 * with itself so the phone pops the setup page up, and serves a page listing
 * the networks it can see with a password field. Stops after 5 minutes, or
 * once a network is saved (then joins it).
 */
typedef void (*prov_status_cb_t)(const char *text, bool done);

bool prov_start(prov_status_cb_t cb);
void prov_stop(void);
const char *prov_ap_ssid(void);
const char *prov_ap_pass(void);
