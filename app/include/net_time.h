#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

/*
 * Wall clock from an NTP server. Syncs once Wi-Fi has an IP address, then every 6 hours.
 * Settings (conf.h): tz, ntp_server
 */

// "Sun" ... "Sat" (tm_wday) and "Jan" ... "Dec" (tm_mon)
extern const char *const net_time_wday_names[7];
extern const char *const net_time_month_names[12];

// Local time. Returns false (tm untouched) until the first sync succeeded
bool net_time_now(struct tm *tm);

// Seconds since 1970 (UTC), -1 until the first sync
int64_t net_time_unix(void);

// The same in milliseconds, for things that move within a second
int64_t net_time_unix_ms(void);

// Read the tz setting again after it changed (it is otherwise read at boot)
void net_time_load_tz(void);

// Is this a UTC offset the tz setting accepts: "+8", "-5", "+5:30"
bool net_time_tz_valid(const char *text);

// Sync now in the background, e.g. after changing ntp_server
void net_time_sync(void);
