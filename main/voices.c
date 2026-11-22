#include "voices.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "esp_log.h"
#include "settings.h"

static const char *TAG = "voices";
#define LOG_PATH "/hist/voice.csv"
#define LOG_MAX (200 * 1024)

static voice_tmpl_t s_t[VOICE_COUNT];
static bool s_enabled;

static void save(void) { settings_set_blob("vprint", s_t, sizeof(s_t)); }

void voices_init(void)
{
    if (!settings_get_blob("vprint", s_t, sizeof(s_t))) {
        memset(s_t, 0, sizeof(s_t));
        uint16_t hz[VOICE_COUNT];   /* the pitch-only recordings from before: kept until re-recorded */
        if (settings_get_blob("voices", hz, sizeof(hz)))
            for (int i = 0; i < VOICE_COUNT; i++) s_t[i].hz = hz[i];
    }
    uint8_t on = 0;
    if (settings_get_blob("vp_on", &on, 1)) s_enabled = on != 0;
    ESP_LOGI(TAG, "vp=%s child %d Hz/%d, dad %d Hz/%d, mom %d Hz/%d", s_enabled ? "on" : "off", s_t[0].hz, s_t[0].n,
             s_t[1].hz, s_t[1].n, s_t[2].hz, s_t[2].n);
}

bool voices_enabled(void) { return s_enabled; }

void voices_set_enabled(bool on)
{
    s_enabled = on;
    uint8_t v = on ? 1 : 0;
    settings_set_blob("vp_on", &v, 1);
}

int voices_enrolled_hz(voice_who_t who) { return who >= 0 && who < VOICE_COUNT ? s_t[who].hz : 0; }
int voices_enrolled_n(voice_who_t who) { return who >= 0 && who < VOICE_COUNT ? s_t[who].n : 0; }

void voices_reset(voice_who_t who)
{
    if (who < 0 || who >= VOICE_COUNT) return;
    memset(&s_t[who], 0, sizeof(s_t[who]));
    save();
}

void voices_reset_all(void)
{
    memset(s_t, 0, sizeof(s_t));
    save();
    ESP_LOGI(TAG, "all voiceprints cleared");
}

void voices_enroll(voice_who_t who, const voice_sample_t *s)
{
    if (who < 0 || who >= VOICE_COUNT) return;
    if (s_t[who].n == 0) s_t[who].hz = 0;   /* a pitch-only recording gives way to the new one */
    vp_enroll(&s_t[who], s);
    save();
    ESP_LOGI(TAG, "%s: %d sentences, %d Hz", voices_key(who), s_t[who].n, s_t[who].hz);
}

float voices_score(voice_who_t who, const voice_sample_t *s)
{
    float sc[VOICE_COUNT];
    vp_classify(s_t, VOICE_COUNT, s, sc);
    return who >= 0 && who < VOICE_COUNT ? sc[who] : 1e9f;
}

static void log_line(const voice_sample_t *s, const float *sc, voice_who_t who)
{
    struct stat st;
    if (stat(LOG_PATH, &st) == 0 && st.st_size > LOG_MAX) return;
    FILE *f = fopen(LOG_PATH, "a");
    if (!f) return;
    fprintf(f, "%lld,%d,%d,%s", (long long)time(NULL), s->hz, s->frames, who == VOICE_UNKNOWN ? "unknown" : voices_key(who));
    for (int i = 0; i < VOICE_COUNT; i++) fprintf(f, ",%.3f", sc[i] > 1e8f ? -1.0f : sc[i]);
    for (int d = 0; d < MFCC_DIMS; d++) fprintf(f, ",%.2f", s->mean[d]);
    fputc('\n', f);
    fclose(f);
}

voice_who_t voices_classify(const voice_sample_t *s)
{
    if (!s_enabled) return VOICE_UNKNOWN;
    float sc[VOICE_COUNT];
    int best = vp_classify(s_t, VOICE_COUNT, s, sc);
    voice_who_t who = best < 0 ? VOICE_UNKNOWN : (voice_who_t)best;
    ESP_LOGI(TAG, "%d Hz, %d blocks: child %.2f dad %.2f mom %.2f -> %s", s->hz, s->frames, sc[0], sc[1], sc[2],
             who == VOICE_UNKNOWN ? "unknown" : voices_key(who));
    log_line(s, sc, who);
    return who;
}

const char *voices_name(voice_who_t who)
{
    static const char *N[] = {"孩子", "爸爸", "妈妈"};
    return who >= 0 && who < VOICE_COUNT ? N[who] : "";
}

const char *voices_key(voice_who_t who)
{
    static const char *K[] = {"child", "dad", "mom"};
    return who >= 0 && who < VOICE_COUNT ? K[who] : "";
}

voice_who_t voices_from_key(const char *key)
{
    for (int i = 0; key && i < VOICE_COUNT; i++)
        if (!strcmp(key, voices_key(i))) return (voice_who_t)i;
    return VOICE_UNKNOWN;
}
