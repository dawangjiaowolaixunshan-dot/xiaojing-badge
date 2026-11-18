#pragma once
/*
 * Timbre of one 32 ms slice of voice: 12 mel-frequency cepstral
 * coefficients (MFCC, c1..c12; c0, the loudness, is left out). Averaged over
 * a sentence they tell voices apart where pitch alone can't (mum vs child).
 * Pure C, no ESP-IDF, so tests/voice can run it on the Mac.
 */
#define MFCC_N 512        /* samples a slice, at 16 kHz */
#define MFCC_DIMS 12

void mfcc_frame(const short *x, float out[MFCC_DIMS]);
