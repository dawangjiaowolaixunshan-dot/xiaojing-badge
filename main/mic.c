#include "mic.h"

#include "esp_codec_dev.h"

#include <math.h>
#include <stdlib.h>

#include "bsp/esp-bsp.h"
#include "esp_codec_dev.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "mic";
#define BLOCK 320           /* 20 ms */
#define GAIN_DB 30.0f

static esp_codec_dev_handle_t s_mic, s_spk;
static volatile mic_sink_t s_sink;
static volatile int s_level;
static TaskHandle_t s_task;

static void mic_task(void *arg)
{
    int16_t *stereo = heap_caps_malloc(BLOCK * 2 * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    int16_t *mono = heap_caps_malloc(BLOCK * sizeof(int16_t), MALLOC_CAP_INTERNAL);
    for (;;) {
        if (!s_sink) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }
        if (esp_codec_dev_read(s_mic, stereo, BLOCK * 2 * sizeof(int16_t)) != ESP_CODEC_DEV_OK) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        int64_t sq = 0;
        for (int i = 0; i < BLOCK; i++) {
            int v = (stereo[2 * i] + stereo[2 * i + 1]) / 2;
            mono[i] = (int16_t)v;
            sq += (int64_t)v * v;
        }
        float rms = sqrtf((float)sq / BLOCK);
        int lvl = rms < 30 ? 0 : (int)(20.0f * log10f(rms / 30.0f) * 2.5f);
        s_level = lvl > 100 ? 100 : lvl;
        mic_sink_t sink = s_sink;
        if (sink) sink(mono, BLOCK);
    }
}

bool mic_init(void)
{
    const i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(MIC_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = BSP_I2S_MCLK,
            .bclk = BSP_I2S_SCLK,
            .ws = BSP_I2S_LCLK,
            .dout = BSP_I2S_DOUT,
            .din = BSP_I2S_DSIN,
        },
    };
    if (bsp_audio_init(&std_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "i2s init failed");
        return false;
    }
    s_mic = bsp_audio_codec_microphone_init();
    if (!s_mic) {
        ESP_LOGE(TAG, "ES7210 init failed");
        return false;
    }
    esp_codec_dev_sample_info_t fs = {.sample_rate = MIC_RATE, .channel = 2, .bits_per_sample = 16};
    if (esp_codec_dev_open(s_mic, &fs) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "open mic failed");
        return false;
    }
    esp_codec_dev_set_in_gain(s_mic, GAIN_DB);
    s_spk = bsp_audio_codec_speaker_init();
    if (!s_spk || esp_codec_dev_open(s_spk, &fs) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "speaker init failed");
        s_spk = NULL;
    } else {
        esp_codec_dev_set_out_vol(s_spk, 75);
    }
    xTaskCreatePinnedToCore(mic_task, "mic", 4096, NULL, 6, &s_task, 1);
    ESP_LOGI(TAG, "mic ready");
    return true;
}

void mic_start(mic_sink_t sink)
{
    s_sink = sink;
    if (s_task) xTaskNotifyGive(s_task);
}

void mic_stop(void)
{
    s_sink = NULL;
    s_level = 0;
}

int mic_level(void) { return s_level; }

esp_codec_dev_handle_t mic_speaker(void) { return s_spk; }
