#include "power.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "driver/rtc_io.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "net.h"
#include "pmu.h"
#include "ui.h"

static const char *TAG = "power";
#define LOG_PATH "/hist/power.csv"
#define LOG_MAX (96 * 1024)

static void log_line(void)
{
    int pct, mv = pmu_battery_mv();
    bool charging, usb;
    if (!pmu_read(&pct, &charging, &usb)) return;
    struct stat st;
    if (!stat(LOG_PATH, &st) && st.st_size > LOG_MAX) remove(LOG_PATH);   /* the Mac keeps the history */
    FILE *f = fopen(LOG_PATH, "a");
    if (!f) return;
    fprintf(f, "%lld,%d,%d,%d,%d,%d,%d\n", (long long)time(NULL), pct, mv, usb, charging, ui_screen_on(), net_up());
    fclose(f);
}

static void power_task(void *arg)
{
    for (;;) {
        log_line();
        vTaskDelay(pdMS_TO_TICKS(60 * 1000));
    }
}

void power_start(void)
{
    xTaskCreate(power_task, "power", 3072, NULL, 1, NULL);
}

void power_dump(void (*line_fn)(const char *line))
{
    FILE *f = fopen(LOG_PATH, "r");
    if (!f) return;
    char line[96];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\n")] = '\0';
        if (line[0]) line_fn(line);
    }
    fclose(f);
}

#define BOOT_GPIO GPIO_NUM_0   /* low while held */
#define PWR_GPIO GPIO_NUM_3    /* high while held */

/* Not used: on this board PWR doesn't wake the chip from deep sleep (GPIO3
 * floats there), and the panel's deep standby didn't come back on wake. */
void power_deep_sleep(int timer_s)
{
    ESP_LOGI(TAG, "deep sleep; PWR or BOOT wakes%s", timer_s ? " (and a timer)" : "");
    log_line();
    ui_prepare_deep_sleep();   /* panel and codecs down */
    pmu_codecs_off();
    net_suspend(true);
    /* PWR drives GPIO3 both ways itself (Muse configures it with no pull); a
     * pull-down here held it low and a press never woke the chip. BOOT needs
     * its pull-up, an RTC one in deep sleep, so the RTC domain stays on. */
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
    rtc_gpio_pullup_dis(PWR_GPIO);
    rtc_gpio_pulldown_dis(PWR_GPIO);
    esp_sleep_enable_ext0_wakeup(PWR_GPIO, 1);
    rtc_gpio_pullup_en(BOOT_GPIO);
    rtc_gpio_pulldown_dis(BOOT_GPIO);
    esp_sleep_enable_ext1_wakeup_io(1ULL << BOOT_GPIO, ESP_EXT1_WAKEUP_ANY_LOW);
    if (timer_s > 0) esp_sleep_enable_timer_wakeup((uint64_t)timer_s * 1000000ULL);
    esp_deep_sleep_start();
}

void power_log_wake(void)
{
    esp_sleep_wakeup_cause_t c = (esp_sleep_wakeup_cause_t)0; (void)c;   /* (deep sleep is off; kept for when it returns) */
    const char *why = c == ESP_SLEEP_WAKEUP_EXT0    ? "PWR"
                      : c == ESP_SLEEP_WAKEUP_EXT1  ? "BOOT"
                      : c == ESP_SLEEP_WAKEUP_TIMER ? "timer"
                                                    : NULL;
    if (why) ESP_LOGI(TAG, "woke from deep sleep by %s", why);
}
