#pragma once
#include <stdbool.h>
#include <stddef.h>

/*
 * The weather where the badge is: the place from its public IP (ip-api.com,
 * once a day, saved), the weather from Open-Meteo (no key) every 30 min.
 */
typedef struct {
    bool valid;
    char city[48];
    float temp, tmax, tmin;
    int code;          /* WMO weather code */
    int humidity;      /* % */
    int rain_chance;   /* % today */
} weather_t;

void weather_start(void);
bool weather_get(weather_t *out);
const char *weather_desc(int code);                 /* 晴, 多云, 小雨 ... */
size_t weather_line(char *buf, size_t cap);         /* for the AI: "上海：晴，12°C，今天 8～15°C，降水概率 10%" */
