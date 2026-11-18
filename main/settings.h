#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Keys and Wi-Fi live in NVS namespace "badge"; Wi-Fi falls back to what the
 * Muse firmware saved (namespace "homehub", plaintext). */
typedef enum { KEY_DEEPSEEK, KEY_BAILIAN, KEY_COUNT } key_id_t;

void settings_init(void);
bool settings_get_key(key_id_t id, char *out, size_t cap);
bool settings_set_key(key_id_t id, const char *value);   /* "" erases */
/* slot 0 = home Wi-Fi, slot 1 = phone hotspot (fallback). An empty ssid erases the slot. */
#define WIFI_SLOTS 8
bool settings_get_wifi(int slot, char *ssid, size_t ssid_cap, char *pass, size_t pass_cap);
bool settings_set_wifi(int slot, const char *ssid, const char *pass);   /* "" empties the slot */
int settings_find_wifi(const char *ssid);                    /* its slot, or -1 */
int settings_add_wifi(const char *ssid, const char *pass);   /* saved in a free slot (or the same SSID's); the slot */
void settings_forget_wifi(int slot);
bool settings_get_blob(const char *name, void *out, size_t len);
bool settings_set_blob(const char *name, const void *data, size_t len);
