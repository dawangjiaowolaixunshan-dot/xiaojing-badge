#include "voices.h"

#include <math.h>

#include "esp_log.h"
#include "settings.h"

static const char *TAG = "voices";
static uint16_t s_hz[VOICE_COUNT];

void voices_init(void)
{
    if (!settings_get_blob("voices", s_hz, sizeof(s_hz))) {
        for (int i = 0; i < VOICE_COUNT; i++) s_hz[i] = 0;
    }
    ESP_LOGI(TAG, "child %d Hz, dad %d Hz, mom %d Hz", s_hz[0], s_hz[1], s_hz[2]);
}

int voices_enrolled_hz(voice_who_t who) { return who >= 0 && who < VOICE_COUNT ? s_hz[who] : 0; }

void voices_enroll(voice_who_t who, int hz)
{
    if (who < 0 || who >= VOICE_COUNT || hz <= 0) return;
    s_hz[who] = (uint16_t)hz;
    settings_set_blob("voices", s_hz, sizeof(s_hz));
}

voice_who_t voices_classify(int hz)
{
    if (hz <= 0) return VOICE_UNKNOWN;
    voice_who_t best = VOICE_UNKNOWN;
    float best_d = 1e9f;
    for (int i = 0; i < VOICE_COUNT; i++) {
        if (!s_hz[i]) continue;
        float d = fabsf(log2f((float)hz / s_hz[i]));   /* in octaves */
        if (d < best_d) {
            best_d = d;
            best = (voice_who_t)i;
        }
    }
    /* Further than about 45% from everyone recorded: someone else, or a bad read. */
    if (best_d > 0.55f) best = VOICE_UNKNOWN;
    ESP_LOGI(TAG, "%d Hz -> %s", hz, best == VOICE_UNKNOWN ? "unknown" : voices_key(best));
    return best;
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
