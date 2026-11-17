#include "settings.h"

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

/* Slot 0 is the home network, slot 1 the phone hotspot. */
static const char *SSID_KEY[2] = {"ssid", "ssid2"};
static const char *PASS_KEY[2] = {"pass", "pass2"};

bool settings_get_wifi(int slot, char *ssid, size_t ssid_cap, char *pass, size_t pass_cap)
{
    pass[0] = '\0';
    if (slot < 0 || slot > 1) {
        return false;
    }
    if (get_str(NS, SSID_KEY[slot], ssid, ssid_cap)) {
        get_str(NS, PASS_KEY[slot], pass, pass_cap);
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
    if (slot < 0 || slot > 1) {
        return false;
    }
    return set_str(SSID_KEY[slot], ssid) && set_str(PASS_KEY[slot], ssid[0] ? pass : "");
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
