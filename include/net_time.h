#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

/*
 * Wall clock from an NTP server. Syncs once Wi-Fi has an IP address, then every 6 hours.
 * SD card config (sdconf.h):
 *   tz=+8              UTC offset, also "-5" or "+5:30" (default +8)
 *   ntp_server=...     default pool.ntp.org
 */

// "Sun" ... "Sat" (tm_wday) and "Jan" ... "Dec" (tm_mon)
extern const char *const net_time_wday_names[7];
extern const char *const net_time_month_names[12];

// Local time. Returns false (tm untouched) until the first sync succeeded
bool net_time_now(struct tm *tm);

// Seconds since 1970 (UTC), -1 until the first sync
int64_t net_time_unix(void);

// Read tz from the config again (it is otherwise read at boot and at every sync)
void net_time_load_tz(void);

// Sync now in the background, e.g. after changing ntp_server
void net_time_sync(void);
