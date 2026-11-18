#include "mfcc.h"

#include <math.h>
#include <stdbool.h>

#define RATE 16000
#define BINS (MFCC_N / 2 + 1)
#define MELS 26
#define F_LO 100.0f
#define F_HI 7000.0f

static float s_win[MFCC_N], s_cos[MFCC_N / 2], s_sin[MFCC_N / 2];
static short s_edge[MELS + 2];   /* FFT bin of each triangle's corners */
static float s_dct[MFCC_DIMS][MELS];
static bool s_ready;

static float mel(float hz) { return 2595.0f * log10f(1.0f + hz / 700.0f); }
static float hz_of(float m) { return 700.0f * (powf(10.0f, m / 2595.0f) - 1.0f); }

static void setup(void)
{
    const float PI = 3.14159265f;
    for (int i = 0; i < MFCC_N; i++) s_win[i] = 0.54f - 0.46f * cosf(2 * PI * i / (MFCC_N - 1));
    for (int i = 0; i < MFCC_N / 2; i++) {
        s_cos[i] = cosf(2 * PI * i / MFCC_N);
        s_sin[i] = -sinf(2 * PI * i / MFCC_N);
    }
    float m0 = mel(F_LO), m1 = mel(F_HI);
    for (int i = 0; i < MELS + 2; i++)
        s_edge[i] = (short)floorf((MFCC_N + 1) * hz_of(m0 + (m1 - m0) * i / (MELS + 1)) / RATE);
    for (int k = 0; k < MFCC_DIMS; k++)
        for (int m = 0; m < MELS; m++) s_dct[k][m] = cosf(PI * (k + 1) * (m + 0.5f) / MELS);
    s_ready = true;
}

static void fft(float *re, float *im)   /* in place, radix 2 */
{
    for (int i = 1, j = 0; i < MFCC_N; i++) {
        int bit = MFCC_N >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j |= bit;
        if (i < j) {
            float t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    for (int len = 2; len <= MFCC_N; len <<= 1) {
        int step = MFCC_N / len;
        for (int i = 0; i < MFCC_N; i += len) {
            for (int k = 0; k < len / 2; k++) {
                float wr = s_cos[k * step], wi = s_sin[k * step];
                float *ar = &re[i + k], *ai = &im[i + k], *br = &re[i + k + len / 2], *bi = &im[i + k + len / 2];
                float tr = *br * wr - *bi * wi, ti = *br * wi + *bi * wr;
                *br = *ar - tr; *bi = *ai - ti;
                *ar += tr; *ai += ti;
            }
        }
    }
}

void mfcc_frame(const short *x, float out[MFCC_DIMS])
{
    static float re[MFCC_N], im[MFCC_N], pw[BINS], lm[MELS];
    if (!s_ready) setup();
    float prev = x[0];
    for (int i = 0; i < MFCC_N; i++) {   /* pre-emphasis, then the window */
        float v = x[i] - 0.97f * prev;
        prev = x[i];
        re[i] = v * s_win[i];
        im[i] = 0;
    }
    fft(re, im);
    for (int i = 0; i < BINS; i++) pw[i] = re[i] * re[i] + im[i] * im[i];
    for (int m = 0; m < MELS; m++) {
        int a = s_edge[m], c = s_edge[m + 1], b = s_edge[m + 2];
        float e = 1e-3f;
        for (int i = a; i < c; i++) e += pw[i] * (i - a) / (float)(c - a);
        for (int i = c; i < b && i < BINS; i++) e += pw[i] * (b - i) / (float)(b - c);
        lm[m] = logf(e);
    }
    for (int k = 0; k < MFCC_DIMS; k++) {
        float s = 0;
        for (int m = 0; m < MELS; m++) s += s_dct[k][m] * lm[m];
        out[k] = s;
    }
}
