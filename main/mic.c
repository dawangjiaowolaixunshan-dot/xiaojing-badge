#include "mic.h"

#include "esp_codec_dev.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "bsp/esp-bsp.h"
#include "esp_codec_dev.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mfcc.h"

static const char *TAG = "mic";
#define BLOCK 320                   /* 20 ms at 16 kHz (MIC_RATE) */
#define HW_RATIO (AUDIO_HW_RATE / MIC_RATE)   /* 3: 48 kHz -> 16 kHz */
#define HW_BLOCK (BLOCK * HW_RATIO) /* 960 stereo frames = 20 ms at 48 kHz */
#define GAIN_DB 30.0f

static esp_codec_dev_handle_t s_mic, s_spk;
static volatile mic_sink_t s_sink;
static volatile int s_level;
static TaskHandle_t s_task;

/* Who's talking: for each voiced 20 ms block, its pitch and the timbre
 * (MFCC) of the last 32 ms; the median pitch and mean timbre at the end. */
#define PITCH_MAX 600
static volatile bool s_voice_on;
static uint16_t *s_pitches;
static volatile int s_npitch;
static vp_acc_t s_acc;
static int16_t s_prev[BLOCK];
static int16_t s_win[MFCC_N];

static void voice_block(const int16_t *cur)
{
    int hz = vp_pitch(s_prev, cur);
    if (!hz) return;
    if (s_npitch < PITCH_MAX) s_pitches[s_npitch++] = (uint16_t)hz;
    memcpy(s_win, s_prev + 2 * BLOCK - MFCC_N, (MFCC_N - BLOCK) * sizeof(int16_t));
    memcpy(s_win + MFCC_N - BLOCK, cur, BLOCK * sizeof(int16_t));
    float c[MFCC_DIMS];
    mfcc_frame(s_win, c);
    vp_acc_add(&s_acc, c);
}

static void mic_task(void *arg)
{
    int16_t *stereo = heap_caps_malloc(HW_BLOCK * 2 * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    int16_t *mono = heap_caps_malloc(BLOCK * sizeof(int16_t), MALLOC_CAP_INTERNAL);
    for (;;) {
        if (!s_sink) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }
        if (esp_codec_dev_read(s_mic, stereo, HW_BLOCK * 2 * sizeof(int16_t)) != ESP_CODEC_DEV_OK) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        int64_t sq = 0;
        for (int i = 0; i < BLOCK; i++) {
            int b = i * HW_RATIO * 2;
            int sum = 0;
            for (int r = 0; r < HW_RATIO; r++) {
                sum += (int)stereo[b + 2 * r] + (int)stereo[b + 2 * r + 1];
            }
            int v = sum / (HW_RATIO * 2);
            mono[i] = (int16_t)v;
            sq += (int64_t)v * v;
        }
        float rms = sqrtf((float)sq / BLOCK);
        int lvl = rms < 30 ? 0 : (int)(20.0f * log10f(rms / 30.0f) * 2.5f);
        s_level = lvl > 100 ? 100 : lvl;
        if (s_voice_on && s_pitches) voice_block(mono);
        memcpy(s_prev, mono, sizeof(s_prev));
        mic_sink_t sink = s_sink;
        if (sink) sink(mono, BLOCK);
    }
}

bool mic_init(void)
{
    const i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_HW_RATE),
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
    esp_codec_dev_sample_info_t fs = {.sample_rate = AUDIO_HW_RATE, .channel = 2, .bits_per_sample = 16};
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
    ESP_LOGI(TAG, "mic ready (I2S %d Hz -> mic %d Hz, tts %d Hz)", AUDIO_HW_RATE, MIC_RATE, TTS_RATE);
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

bool mic_active(void) { return s_sink != NULL; }

int mic_level(void) { return s_level; }

esp_codec_dev_handle_t mic_speaker(void) { return s_spk; }

void mic_voice_begin(void)
{
    if (!s_pitches) s_pitches = heap_caps_malloc(PITCH_MAX * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    s_npitch = 0;
    memset(&s_acc, 0, sizeof(s_acc));
    s_voice_on = true;
}

static int cmp_u16(const void *a, const void *b) { return *(const uint16_t *)a - *(const uint16_t *)b; }

bool mic_voice_end(voice_sample_t *out)
{
    s_voice_on = false;
    vTaskDelay(pdMS_TO_TICKS(25));   /* let a block in progress finish */
    int n = s_npitch, hz = 0;
    if (s_pitches && n >= 10) {   /* under 0.2 s of voice: no pitch */
        qsort(s_pitches, n, sizeof(uint16_t), cmp_u16);
        hz = s_pitches[n / 2];
    }
    bool ok = vp_acc_done(&s_acc, hz, out);
    ESP_LOGI(TAG, "voice: %d Hz over %d voiced blocks%s", hz, out->frames, ok ? "" : " (too little)");
    return ok;
}
