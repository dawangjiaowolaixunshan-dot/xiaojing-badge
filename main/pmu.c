#include "pmu.h"

#include "bsp/esp-bsp.h"
#include "driver/i2c_master.h"
#include "esp_log.h"

static const char *TAG = "pmu";
#define AXP2101_ADDR 0x34
#define REG_STATUS1 0x00      /* bit5 VBUS good, bit3 battery present */
#define REG_STATUS2 0x01      /* bits 6:5 = 01 while charging */
#define REG_COMMON_CFG 0x10   /* bit2: shut down after 16 s of PWR; bits 1:0 are commands, never write 1 */
#define REG_PWROFF_EN 0x22    /* bit1: power off when PWR is held past the hold-off */
#define REG_IRQ_LEVEL 0x27    /* bits 3:2: hold-off; 11 = 10 s */
#define REG_ADC_ENABLE 0x30   /* bit0 battery voltage */
#define REG_BAT_PERCENT 0xA4
#define REG_INTEN1 0x40
#define REG_INTSTS1 0x48

static i2c_master_dev_handle_t s_dev;
static bool s_16s_was_on;

static esp_err_t rd(uint8_t reg, uint8_t *v) { return i2c_master_transmit_receive(s_dev, &reg, 1, v, 1, 50); }
static esp_err_t wr(uint8_t reg, uint8_t v)
{
    uint8_t b[2] = {reg, v};
    return i2c_master_transmit(s_dev, b, 2, 50);
}

void pmu_init(void)
{
    if (bsp_i2c_init() != ESP_OK) return;
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = AXP2101_ADDR,
        .scl_speed_hz = 400000,
    };
    if (i2c_master_bus_add_device(bsp_i2c_get_handle(), &cfg, &s_dev) != ESP_OK) {
        s_dev = NULL;
        return;
    }
    uint8_t v;
    if (rd(REG_IRQ_LEVEL, &v) != ESP_OK || wr(REG_IRQ_LEVEL, (v & ~0x0c) | (3u << 2)) != ESP_OK) {
        ESP_LOGW(TAG, "AXP2101 not answering: a long PWR hold may power off");
        s_dev = NULL;
        return;
    }
    /* No one reads its interrupts; a pending one would hold the IRQ line low. */
    for (int i = 0; i < 3; i++) {
        wr(REG_INTEN1 + i, 0);
        wr(REG_INTSTS1 + i, 0xff);
    }
    if (rd(REG_ADC_ENABLE, &v) == ESP_OK) wr(REG_ADC_ENABLE, v | 0x01);   /* the fuel gauge needs it */
    ESP_LOGI(TAG, "PWR hold-off 10 s");
}

void pmu_key_shutdown(bool on)
{
    uint8_t v;
    if (!s_dev) return;
    if (rd(REG_PWROFF_EN, &v) == ESP_OK) wr(REG_PWROFF_EN, on ? v | 0x02 : v & ~0x02);
    if (rd(REG_COMMON_CFG, &v) == ESP_OK) {
        v &= ~0x03;
        if (!on && (v & 0x04)) {
            s_16s_was_on = wr(REG_COMMON_CFG, v & ~0x04) == ESP_OK;
        } else if (on && s_16s_was_on) {
            s_16s_was_on = wr(REG_COMMON_CFG, v | 0x04) != ESP_OK;
        }
    }
}

bool pmu_read(int *pct, bool *charging, bool *usb)
{
    uint8_t s1, s2, p;
    *pct = -1;
    *charging = *usb = false;
    if (!s_dev || rd(REG_STATUS1, &s1) != ESP_OK || rd(REG_STATUS2, &s2) != ESP_OK) return false;
    bool battery = s1 & 0x08;
    *usb = s1 & 0x20;
    *charging = battery && ((s2 >> 5) & 0x3) == 0x1;
    if (battery && rd(REG_BAT_PERCENT, &p) == ESP_OK && p <= 100) *pct = p;
    return true;
}

void pmu_power_off(void)
{
    uint8_t v;
    if (s_dev && rd(REG_COMMON_CFG, &v) == ESP_OK) wr(REG_COMMON_CFG, (v & ~0x03) | 0x01);
}
