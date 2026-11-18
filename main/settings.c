#include "settings.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "settings";
static const char *NS = "badge";
static const char *KEY_NAMES[KEY_COUNT] = {"ds_key", "bl_key"};

void settings_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS unreadable (%s): erasing", esp_err_to_name(err));
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

static bool get_str(const char *ns, const char *key, char *out, size_t cap)
{
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t len = cap;
    esp_err_t err = nvs_get_str(h, key, out, &len);
    nvs_close(h);
    return err == ESP_OK && out[0];
}

static bool set_str(const char *key, const char *value)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    esp_err_t err = value[0] ? nvs_set_str(h, key, value) : nvs_erase_key(h, key);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err == ESP_OK;
}

bool settings_get_key(key_id_t id, char *out, size_t cap)
{
    return id < KEY_COUNT && get_str(NS, KEY_NAMES[id], out, cap);
}

bool settings_set_key(key_id_t id, const char *value)
{
    return id < KEY_COUNT && set_str(KEY_NAMES[id], value);
}

/* Up to WIFI_SLOTS networks. Slot 0 was "home" and slot 1 "the phone
 * hotspot" (keys ssid/pass and ssid2/pass2, kept as they were); the rest are
 * ssid3/pass3 .. ssid8/pass8. */
static void wifi_keys(int slot, char *sk, char *pk)
{
    if (slot == 0) {
        strcpy(sk, "ssid");
        strcpy(pk, "pass");
    } else {
        sprintf(sk, "ssid%d", slot + 1);
        sprintf(pk, "pass%d", slot + 1);
    }
}

bool settings_get_wifi(int slot, char *ssid, size_t ssid_cap, char *pass, size_t pass_cap)
{
    char sk[8], pk[8];
    pass[0] = '\0';
    if (slot < 0 || slot >= WIFI_SLOTS) return false;
    wifi_keys(slot, sk, pk);
    if (get_str(NS, sk, ssid, ssid_cap)) {
        get_str(NS, pk, pass, pass_cap);
        return true;
    }
    if (slot == 0 && get_str("homehub", "ssid", ssid, ssid_cap)) {   /* left by the Muse firmware */
        get_str("homehub", "password", pass, pass_cap);
        return true;
    }
    return false;
}

bool settings_set_wifi(int slot, const char *ssid, const char *pass)
{
    char sk[8], pk[8];
    if (slot < 0 || slot >= WIFI_SLOTS) return false;
    wifi_keys(slot, sk, pk);
    return set_str(sk, ssid) && set_str(pk, pass);
}

int settings_find_wifi(const char *ssid)
{
    char s[33], p[65];
    for (int i = 0; i < WIFI_SLOTS; i++) {
        if (settings_get_wifi(i, s, sizeof(s), p, sizeof(p)) && !strcmp(s, ssid)) {
            memset(p, 0, sizeof(p));
            return i;
        }
    }
    memset(p, 0, sizeof(p));
    return -1;
}

int settings_add_wifi(const char *ssid, const char *pass)
{
    char s[33], p[65];
    int slot = settings_find_wifi(ssid);   /* a known one: new password */
    for (int i = 0; slot < 0 && i < WIFI_SLOTS; i++) {
        if (!settings_get_wifi(i, s, sizeof(s), p, sizeof(p))) slot = i;
    }
    memset(p, 0, sizeof(p));
    if (slot < 0) slot = WIFI_SLOTS - 1;   /* full: the last one makes way */
    return settings_set_wifi(slot, ssid, pass) ? slot : -1;
}

void settings_forget_wifi(int slot)
{
    settings_set_wifi(slot, "", "");
    if (slot == 0) {   /* and the network Muse saved, which slot 0 falls back to */
        nvs_handle_t h;
        if (nvs_open("homehub", NVS_READWRITE, &h) == ESP_OK) {
            nvs_erase_key(h, "ssid");
            nvs_erase_key(h, "password");
            nvs_commit(h);
            nvs_close(h);
        }
    }
}

bool settings_get_blob(const char *name, void *out, size_t len)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t got = len;
    esp_err_t err = nvs_get_blob(h, name, out, &got);
    nvs_close(h);
    return err == ESP_OK && got == len;
}

bool settings_set_blob(const char *name, const void *data, size_t len)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    esp_err_t err = nvs_set_blob(h, name, data, len);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err == ESP_OK;
}
