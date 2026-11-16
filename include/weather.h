#pragma once

#include <stdbool.h>
#include <stdint.h>

/*
 * Current weather and a 3 day forecast from wttr.in (plain HTTP, metric units).
 * Fetched in the background once Wi-Fi is online, then every 30 minutes.
 * SD card config (sdconf.h):
 *   weather_location=Taipei     city, "lat,lon" or anything wttr.in accepts (default: by IP)
 * Shell: weather / weather update
 */

#define WEATHER_DAYS        3
#define WEATHER_DESC_LEN    32
#define WEATHER_AREA_LEN    24

typedef enum {
    WEATHER_SUNNY,
    WEATHER_PARTLY,
    WEATHER_CLOUDY,
    WEATHER_FOG,
    WEATHER_RAIN,
    WEATHER_SNOW,
    WEATHER_THUNDER,
} weather_kind_t;

typedef struct {
    int wday;               // 0 = Sunday
    int max_c;
    int min_c;
    int rain;               // highest chance of rain of the day, %
    weather_kind_t kind;    // weather at noon
} weather_day_t;

typedef struct {
    uint32_t generation;    // goes up with every successful update
    int64_t updated_ms;     // k_uptime_get() of the update
    int temp_c;
    int feels_c;
    int humidity;
    int wind_kmph;
    char wind_dir[4];       // "ENE"
    weather_kind_t kind;
    char desc[WEATHER_DESC_LEN];
    char area[WEATHER_AREA_LEN];
    int day_count;
    weather_day_t days[WEATHER_DAYS];
} weather_t;

typedef enum {
    WEATHER_OFFLINE,        // waiting for Wi-Fi
    WEATHER_LOADING,
    WEATHER_READY,          // last update succeeded
    WEATHER_FAILED,         // last update failed, retries in a minute
} weather_state_t;

// Copy the last good result. Returns false when there is none yet
bool weather_get(weather_t *out);

weather_state_t weather_state(void);

// Error of the last failed update (negative errno)
int weather_error(void);

// Update now in the background (no-op while offline)
void weather_update(void);
