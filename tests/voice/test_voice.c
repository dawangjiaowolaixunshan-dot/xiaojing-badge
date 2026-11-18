/* Host test of the voice ID: cc -O2 -I ../../main test_voice.c ../../main/vp.c ../../main/mfcc.c -lm
 * usage: test_voice <enrolled voices: a,b,c> <sentences to enroll> <wav dir>
 * Wav files <dir>/<voice>_<i>.wav, 16 kHz mono 16-bit. Enrolls each voice from
 * its first sentences, then classifies the rest (and any voice not enrolled). */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vp.h"

static int load(const char *path, short **out)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *b = malloc(n);
    fread(b, 1, n, f);
    fclose(f);
    long at = 12;   /* find the "data" chunk */
    while (at + 8 < n && memcmp(b + at, "data", 4)) at += 8 + *(int *)(b + at + 4);
    int len = *(int *)(b + at + 4) / 2;
    *out = malloc(len * 2);
    memcpy(*out, b + at + 8, len * 2);
    free(b);
    return len;
}

static int cmp(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }

/* As mic.c does it: 20 ms blocks; a voiced block gives a pitch and an MFCC
 * over the last 32 ms. */
static int sample_of(const char *path, voice_sample_t *s)
{
    short *pcm;
    int n = load(path, &pcm);
    if (!n) return 0;
    vp_acc_t acc = {0};
    int pitches[2000], np = 0;
    short win[MFCC_N];
    for (int at = VP_BLOCK; at + VP_BLOCK <= n; at += VP_BLOCK) {
        int hz = vp_pitch(pcm + at - VP_BLOCK, pcm + at);
        if (!hz) continue;
        if (np < 2000) pitches[np++] = hz;
        if (at + VP_BLOCK >= MFCC_N) {
            memcpy(win, pcm + at + VP_BLOCK - MFCC_N, sizeof(win));
            float c[MFCC_DIMS];
            mfcc_frame(win, c);
            vp_acc_add(&acc, c);
        }
    }
    free(pcm);
    int hz = 0;
    if (np >= 10) {
        qsort(pitches, np, sizeof(int), cmp);
        hz = pitches[np / 2];
    }
    return vp_acc_done(&acc, hz, s);
}

int main(int argc, char **argv)
{
    char names[8][32];
    int nv = 0;
    for (char *p = strtok(argv[1], ","); p && nv < 8; p = strtok(NULL, ",")) strcpy(names[nv++], p);
    int nenroll = atoi(argv[2]);
    const char *dir = argv[3];
    voice_tmpl_t t[8] = {0}, tp[8] = {0};
    char path[512];
    for (int v = 0; v < nv; v++) {
        for (int i = 0; i < nenroll; i++) {
            voice_sample_t s;
            snprintf(path, sizeof(path), "%s/%s_%d.wav", dir, names[v], i);
            if (sample_of(path, &s)) vp_enroll(&t[v], &s);
        }
        tp[v].hz = t[v].hz;   /* pitch only, as before */
        printf("%-10s enrolled: %d sentences, %d Hz\n", names[v], t[v].n, t[v].hz);
    }
    /* Every wav in the dir that wasn't used to enroll. */
    DIR *d = opendir(dir);
    struct dirent *e;
    int right = 0, total = 0, right_p = 0;
    while ((e = readdir(d))) {
        char who[32];
        int idx;
        if (sscanf(e->d_name, "%31[^_]_%d.wav", who, &idx) != 2) continue;
        int truth = -1;
        for (int v = 0; v < nv; v++) if (!strcmp(who, names[v])) truth = v;
        if (truth >= 0 && idx < nenroll) continue;
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        voice_sample_t s;
        if (!sample_of(path, &s)) { printf("%-16s too short\n", e->d_name); continue; }
        float sc[8], scp[8];
        int got = vp_classify(t, nv, &s, sc);
        int gotp = vp_classify(tp, nv, &s, scp);
        total++;
        right += got == truth;
        right_p += gotp == truth;
        printf("%-16s %3d Hz -> %-10s (pitch only: %-10s) scores", e->d_name, s.hz,
               got >= 0 ? names[got] : "unknown", gotp >= 0 ? names[gotp] : "unknown");
        for (int v = 0; v < nv; v++) printf(" %.2f", sc[v]);
        printf("\n");
    }
    closedir(d);
    printf("\nwith timbre: %d/%d right;  pitch only: %d/%d right\n", right, total, right_p, total);
    return 0;
}
