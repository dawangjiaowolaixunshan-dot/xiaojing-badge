#pragma once
/*
 * A voice from one sentence: its median pitch and the mean (and spread) of
 * its MFCCs. A person's template is the average of a few such sentences.
 * Pure C, shared with the Mac test in tests/voice.
 */
#include <stdint.h>

#include "mfcc.h"

typedef struct {
    uint16_t hz, frames;
    float mean[MFCC_DIMS], var[MFCC_DIMS];
} voice_sample_t;

typedef struct {
    uint16_t hz;
    uint8_t n, _pad;              /* sentences averaged in; 0 = pitch only (old recording) */
    float mean[MFCC_DIMS], var[MFCC_DIMS];
} voice_tmpl_t;

typedef struct {
    double sum[MFCC_DIMS], sq[MFCC_DIMS];
    int n;
} vp_acc_t;

#define VP_MIN_FRAMES 15          /* 0.3 s of voice */
#define VP_UNKNOWN 1.0f           /* a best score above this: someone else. Loose on purpose: in a family of three, "unknown" for a parent is worse than a guest taken for one */

void vp_acc_add(vp_acc_t *a, const float c[MFCC_DIMS]);
int vp_acc_done(const vp_acc_t *a, int hz, voice_sample_t *out);   /* 0 if too little voice */
void vp_enroll(voice_tmpl_t *t, const voice_sample_t *s);
float vp_score(const voice_tmpl_t *t, const voice_sample_t *s, const float var[MFCC_DIMS]);
/* The best match among n templates, or -1; each one's score in scores[] (lower is closer). */
int vp_classify(const voice_tmpl_t *t, int n, const voice_sample_t *s, float *scores);

#define VP_BLOCK 320              /* 20 ms at 16 kHz */
/* Pitch over the last 40 ms (two blocks), Hz; 0 if not voiced. */
int vp_pitch(const short *prev, const short *cur);
