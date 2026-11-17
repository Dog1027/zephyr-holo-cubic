#pragma once

#include <stddef.h>

/*
 * The user's settings, kept in the flash settings (prefs.h) so they work without an SD card:
 *
 *   wifi_ssid          Wi-Fi network name
 *   wifi_psk           Wi-Fi password, 8 to 64 characters (not set: open network)
 *   tz                 UTC offset: "+8", "-5", "+5:30" (default +8)
 *   ntp_server         default pool.ntp.org
 *   weather_location   city name or "lat,lon" (default: guessed from the IP address)
 *
 * Ways to change them: the shell ("config set tz +9", "wlan set ..."), or /config.txt on the
 * SD card with one "key=value" per line. That file is an import, not a second copy: at boot,
 * and when the Share app saves it, every known key in it is checked, stored here and then
 * removed from the file. Keys with a bad value stay in the file and are logged; comments and
 * unknown keys are left alone.
 *
 * Modules read their values with conf_get() when they need them, nothing reads the file.
 */

#define CONF_WIFI_SSID          "wifi_ssid"
#define CONF_WIFI_PSK           "wifi_psk"
#define CONF_TZ                 "tz"
#define CONF_NTP_SERVER         "ntp_server"
#define CONF_WEATHER_LOCATION   "weather_location"

// Longest value of any key, without the NUL
#define CONF_VALUE_MAX          64

// Copy the value into buf (NUL terminated). Returns its length, 0 (empty string) when it is
// not set, -ENOENT for a key that is not in the list above
int conf_get(const char *key, char *buf, size_t len);

// Store a value; NULL or "" removes it. Returns 1 if it changed, 0 if it was already that,
// -ENOENT for an unknown key, -EINVAL for a value the key does not accept.
// The module using it is not told: call wlan_connect() / net_time_load_tz() where it matters
int conf_set(const char *key, const char *value);

// Import /config.txt in the background (see above) and tell the modules what changed:
// new Wi-Fi settings reconnect, a new tz applies at once
void conf_import(void);
