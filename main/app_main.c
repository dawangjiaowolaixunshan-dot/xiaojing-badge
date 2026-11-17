/*
 * 电子吧唧: DeepSeek voice chat and 背诗 on the Waveshare ESP32-S3-Touch-
 * AMOLED-1.75C. Keys and Wi-Fi come in over USB (tools/badge_setup.py).
 */
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "chat.h"
#include "console.h"
#include "history.h"
#include "mic.h"
#include "net.h"
#include "pmu.h"
#include "prefs.h"
#include "pet.h"
#include "poems.h"
#include "settings.h"
#include "tts.h"
#include "ui.h"
#include "power.h"
#include "voices.h"
#include "weather.h"

static const char *TAG = "app";
#define BOOT_GPIO GPIO_NUM_0

/*
 * The 1.75C's side keys: PWR talks (GPIO3 reads high while it's held; the
 * chip powers off on a long hold, so that's switched off for the talk), and
 * BOOT (GPIO0, low while held) goes back.
 */
#define PWR_GPIO GPIO_NUM_3

typedef struct {
    gpio_num_t pin;
    int active;
    bool pressed;
    int same;
} key_t;

static bool key_changed(key_t *k)
{
    bool now = gpio_get_level(k->pin) == k->active;
    if (now == k->pressed) {
        k->same = 0;
        return false;
    }
    if (++k->same < 3) return false;   /* 45 ms debounce */
    k->pressed = now;
    k->same = 0;
    return true;
}

static void button_task(void *arg)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << BOOT_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&io);
    io.pin_bit_mask = 1ULL << PWR_GPIO;
    io.pull_up_en = GPIO_PULLUP_DISABLE;
    io.pull_down_en = GPIO_PULLDOWN_ENABLE;
    gpio_config(&io);
    key_t boot = {BOOT_GPIO, 0, false, 0};
    key_t pwr = {PWR_GPIO, 1, gpio_get_level(PWR_GPIO) == 1, 0};   /* still held from power-on: not a talk */
    bool pwr_armed = !pwr.pressed;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(15));
        if (key_changed(&boot)) ui_boot_button(boot.pressed);
        if (key_changed(&pwr)) {
            if (!pwr_armed) {
                pwr_armed = !pwr.pressed;   /* the power-on hold ended */
                continue;
            }
            pmu_key_shutdown(!pwr.pressed);
            ui_talk_button(pwr.pressed);
        }
    }
}

static void heartbeat_task(void *arg)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(30000));
        ESP_LOGI(TAG, "heap int=%uK (min %uK) dma=%uK psram=%uK net=%s",
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                 (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024),
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_DMA) / 1024),
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024), net_up() ? "up" : "down");
    }
}

void app_main(void)
{
    console_init_early();
    power_log_wake();
    /* 240 MHz when busy, 80 MHz when idle; no light sleep (it would cut the
     * USB console and the display's refresh). */
    esp_pm_config_t pm = {.max_freq_mhz = 240, .min_freq_mhz = 80, .light_sleep_enable = false};
    if (esp_pm_configure(&pm) != ESP_OK) ESP_LOGW(TAG, "no frequency scaling");
    else ESP_LOGI(TAG, "CPU 80-240 MHz");
    settings_init();
    poems_init();
    pet_init();
    history_init();
    chat_restore();
    history_recall_init();
    voices_init();
    ui_start();
    pmu_init();
    console_start();
    net_start();
    weather_start();
    power_start();
    if (!mic_init()) {
        ESP_LOGE(TAG, "no microphone: DeepSeek can't listen");
    }
    tts_init();
    prefs_apply();   /* again now the speaker is up: its volume */
    xTaskCreate(button_task, "buttons", 3072, NULL, 4, NULL);
    xTaskCreate(heartbeat_task, "heartbeat", 3072, NULL, 1, NULL);
    ESP_LOGI(TAG, "up");
}
