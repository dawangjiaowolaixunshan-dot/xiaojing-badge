#pragma once
#include "vp.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* 16 kHz mono 16-bit from the two ES7210 microphones, mixed. */
#define MIC_RATE 16000
typedef void (*mic_sink_t)(const int16_t *pcm, size_t samples);

bool mic_init(void);
void mic_start(mic_sink_t sink);   /* calls sink from the mic task until mic_stop() */
void mic_stop(void);
int mic_level(void);               /* 0..100, the last block's loudness */
void mic_voice_begin(void);                 /* start noting who's talking (while the mic runs) */
bool mic_voice_end(voice_sample_t *out);    /* pitch and timbre; false if too little voice */

/* The ES8311 speaker, opened on the same I2S at MIC_RATE, 2 channels (NULL if absent). */
#include "esp_codec_dev.h"
esp_codec_dev_handle_t mic_speaker(void);
