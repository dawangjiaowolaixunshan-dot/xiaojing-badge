#include "vp.h"

#include <math.h>
#include <string.h>

void vp_acc_add(vp_acc_t *a, const float c[MFCC_DIMS])
{
    for (int d = 0; d < MFCC_DIMS; d++) {
        a->sum[d] += c[d];
        a->sq[d] += (double)c[d] * c[d];
    }
    a->n++;
}

int vp_acc_done(const vp_acc_t *a, int hz, voice_sample_t *out)
{
    memset(out, 0, sizeof(*out));
    out->hz = (uint16_t)hz;
    out->frames = (uint16_t)a->n;
    if (a->n < VP_MIN_FRAMES) return 0;
    for (int d = 0; d < MFCC_DIMS; d++) {
        double m = a->sum[d] / a->n;
        out->mean[d] = (float)m;
        out->var[d] = (float)(a->sq[d] / a->n - m * m);
    }
    return 1;
}

void vp_enroll(voice_tmpl_t *t, const voice_sample_t *s)
{
    if (s->frames < VP_MIN_FRAMES) return;
    float w = 1.0f / (t->n < 5 ? t->n + 1 : 5);   /* an average of the first five, then a slow update */
    for (int d = 0; d < MFCC_DIMS; d++) {
        t->mean[d] += w * (s->mean[d] - t->mean[d]);
        t->var[d] += w * (s->var[d] - t->var[d]);
    }
    if (s->hz) t->hz = t->n == 0 || !t->hz ? s->hz : (uint16_t)(t->hz + w * ((float)s->hz - t->hz));
    if (t->n < 255) t->n++;
}

static float pitch_term(int a, int b)
{
    if (!a || !b) return 0.3f;   /* no pitch to compare: neither for nor against */
    float oct = log2f((float)a / b);
    /* 0.35 octave apart adds 0.1: a question asked aloud runs a third of an
     * octave above the same voice reading a line (dad: 128 Hz read, 166 asked). */
    return 0.1f * (oct / 0.35f) * (oct / 0.35f);
}

float vp_score(const voice_tmpl_t *t, const voice_sample_t *s, const float var[MFCC_DIMS])
{
    float p = pitch_term(s->hz, t->hz);
    if (t->n == 0) return p * 2;   /* pitch is all there is */
    float d = 0;
    for (int k = 0; k < MFCC_DIMS; k++) {
        float diff = s->mean[k] - t->mean[k];
        d += diff * diff / (var[k] > 1e-3f ? var[k] : 1e-3f);
    }
    return d / MFCC_DIMS + p;
}

int vp_classify(const voice_tmpl_t *t, int n, const voice_sample_t *s, float *scores)
{
    /* Spread of a slice's MFCCs, pooled over everyone recorded: the scale
     * that differences between voices are measured in. */
    float var[MFCC_DIMS] = {0};
    int m = 0;
    for (int i = 0; i < n; i++) {
        if (!t[i].n) continue;
        for (int d = 0; d < MFCC_DIMS; d++) var[d] += t[i].var[d];
        m++;
    }
    for (int d = 0; d < MFCC_DIMS; d++) var[d] = m ? var[d] / m : s->var[d];
    int best = -1;
    float best_s = 1e9f;
    for (int i = 0; i < n; i++) {
        scores[i] = 1e9f;
        if (!t[i].n && !t[i].hz) continue;   /* not recorded */
        if (t[i].n && s->frames < VP_MIN_FRAMES) continue;
        scores[i] = vp_score(&t[i], s, var);
        if (scores[i] < best_s) {
            best_s = scores[i];
            best = i;
        }
    }
    return best_s <= VP_UNKNOWN ? best : -1;
}

int vp_pitch(const short *prev, const short *cur)
{
    /* Down to 8 kHz first (pairs averaged): pitch needs no more, and the
     * autocorrelation costs a quarter. */
    enum { N = VP_BLOCK, RATE = 8000, LMIN = RATE / 450, LMAX = RATE / 70 };
    static float x[N], r[LMAX + 1];
    float e = 0;
    for (int j = 0; j < N; j++) {
        int i = 2 * j;
        float v = i < VP_BLOCK ? 0.5f * (prev[i] + prev[i + 1]) : 0.5f * (cur[i - VP_BLOCK] + cur[i - VP_BLOCK + 1]);
        x[j] = v;
        e += v * v;
    }
    if (e < N * 200.0f * 200.0f) return 0;   /* too quiet to be speech */
    float best = 0;
    int lag = 0;
    for (int l = LMIN; l <= LMAX; l++) {
        float xy = 0, xx = 0, yy = 0;
        for (int i = 0; i + l < N; i++) {
            xy += x[i] * x[i + l];
            xx += x[i] * x[i];
            yy += x[i + l] * x[i + l];
        }
        r[l] = xx > 0 && yy > 0 ? xy / sqrtf(xx * yy) : 0;
        if (r[l] > best) {
            best = r[l];
            lag = l;
        }
    }
    if (best < 0.6f || !lag) return 0;   /* not voiced */
    /* A peak at twice the period looks as good; prefer the shorter one. */
    if (lag / 2 >= LMIN && r[lag / 2] > 0.85f * best) lag /= 2;
    return RATE / lag;
}
