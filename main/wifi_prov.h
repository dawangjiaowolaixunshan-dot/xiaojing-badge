#pragma once
#include <stdbool.h>

/*
 * Optional fallback: adding a Wi-Fi from a phone browser. The badge opens a
 * temporary access point (XiaoJing-NNNN, random 8-digit WPA2 password),
 * answers every DNS query with itself, and serves a setup page. Stops after
 * 5 minutes, on back, or once a network is saved (then always returns to pure
 * STA receiver mode and joins the saved network).
 */
typedef void (*prov_status_cb_t)(const char *text, bool done);

bool prov_start(prov_status_cb_t cb);
void prov_stop(void);
bool prov_active(void);
const char *prov_ap_ssid(void);
const char *prov_ap_pass(void);
