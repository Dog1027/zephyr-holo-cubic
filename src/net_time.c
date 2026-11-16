#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/net_event.h>
#include <zephyr/net/sntp.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/clock.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "net_job.h"
#include "net_time.h"
#include "sdconf.h"

LOG_MODULE_REGISTER(net_time, CONFIG_CUBE_LOG_LEVEL);

#define KEY_TZ          "tz"
#define KEY_SERVER      "ntp_server"
#define DEFAULT_TZ_S    (8 * 3600)
#define DEFAULT_SERVER  "pool.ntp.org"
#define SNTP_TIMEOUT_MS 3000
#define RESYNC_DELAY    K_HOURS(6)
#define RETRY_DELAY     K_SECONDS(15)

const char *const net_time_wday_names[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
const char *const net_time_month_names[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                              "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

static void sync_work_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(sync_work, sync_work_fn);
static struct net_mgmt_event_callback l4_cb;

static bool synced;
static int32_t tz_offset_s = DEFAULT_TZ_S;

// "+8"、"-5"、"+5:30"、"8" 都接受，格式不對回傳 false
static bool parse_tz(const char *s, int32_t *offset_s)
{
    char *end;
    long hours = strtol(s, &end, 10);
    long minutes = 0;

    if (end == s || hours < -12 || hours > 14) {
        return false;
    }
    if (*end == ':') {
        const char *m = end + 1;
        minutes = strtol(m, &end, 10);
        if (end == m || minutes < 0 || minutes > 59) {
            return false;
        }
    }
    if (*end != '\0') {
        return false;
    }
    int32_t sign = (s[0] == '-') ? -1 : 1;
    *offset_s = (int32_t)hours * 3600 + sign * (int32_t)minutes * 60;
    return true;
}

void net_time_load_tz(void)
{
    char buf[8];
    int32_t offset = DEFAULT_TZ_S;

    if (sdconf_get(KEY_TZ, buf, sizeof(buf)) > 0 && !parse_tz(buf, &offset)) {
        LOG_WRN("Bad %s=%s in %s, using +8", KEY_TZ, buf, SDCONF_PATH);
        offset = DEFAULT_TZ_S;
    }
    tz_offset_s = offset;
}

int64_t net_time_unix(void)
{
    struct timespec ts;

    if (!synced || sys_clock_gettime(SYS_CLOCK_REALTIME, &ts) < 0) {
        return -1;
    }
    return ts.tv_sec;
}

bool net_time_now(struct tm *tm)
{
    int64_t now = net_time_unix();

    if (now < 0) {
        return false;
    }
    time_t local = (time_t)(now + tz_offset_s);
    return gmtime_r(&local, tm) != NULL;
}

static void sync_work_fn(struct k_work *work)
{
    char server[64];
    struct sntp_time sntp;

    if (sdconf_get(KEY_SERVER, server, sizeof(server)) <= 0) {
        strcpy(server, DEFAULT_SERVER);
    }
    net_time_load_tz();

    int ret = sntp_simple(server, SNTP_TIMEOUT_MS, &sntp);
    if (ret < 0) {
        LOG_WRN("SNTP %s failed (%d), retrying in 15 s", server, ret);
        k_work_reschedule_for_queue(net_job_queue(), &sync_work, RETRY_DELAY);
        return;
    }

    // fraction 是 1/2^32 秒
    struct timespec ts = {
        .tv_sec = (time_t)sntp.seconds,
        .tv_nsec = (long)(((uint64_t)sntp.fraction * NSEC_PER_SEC) >> 32),
    };
    ret = sys_clock_settime(SYS_CLOCK_REALTIME, &ts);
    if (ret < 0) {
        LOG_ERR("Failed to set the clock (%d)", ret);
        return;
    }
    synced = true;

    struct tm tm;
    net_time_now(&tm);
    LOG_INF("Time synced from %s: %04d-%02d-%02d %02d:%02d:%02d", server, tm.tm_year + 1900,
            tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    k_work_reschedule_for_queue(net_job_queue(), &sync_work, RESYNC_DELAY);
}

void net_time_sync(void)
{
    k_work_reschedule_for_queue(net_job_queue(), &sync_work, K_NO_WAIT);
}

// 連上網路就對時（重新連上也會再對一次）。用 connection manager 的 L4_CONNECTED，
// 不用 IPV4_ADDR_ADD：位址加上去的時候介面可能還沒 up，馬上送封包會 -ENETUNREACH
static void l4_event_handler(struct net_mgmt_event_callback *cb, uint64_t event,
                             struct net_if *iface)
{
    net_time_sync();
}

static int net_time_init(void)
{
    net_time_load_tz();
    net_mgmt_init_event_callback(&l4_cb, l4_event_handler, NET_EVENT_L4_CONNECTED);
    net_mgmt_add_event_callback(&l4_cb);
    return 0;
}

// 要在 SD 卡掛載之後才讀得到 tz
SYS_INIT(net_time_init, APPLICATION, CONFIG_WLAN_INIT_PRIORITY);

/* ---------- shell ---------- */

static int _ntp_status(const struct shell *shell, size_t argc, char **argv)
{
    struct tm tm;
    int32_t tz = tz_offset_s;
    char tz_text[12];

    snprintf(tz_text, sizeof(tz_text), "%c%d:%02d", tz < 0 ? '-' : '+', abs(tz) / 3600,
             abs(tz) / 60 % 60);
    if (!net_time_now(&tm)) {
        shell_print(shell, "Not synced yet (needs Wi-Fi), tz %s", tz_text);
        return 0;
    }
    shell_print(shell, "%04d-%02d-%02d %02d:%02d:%02d (UTC%s)", tm.tm_year + 1900, tm.tm_mon + 1,
                tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, tz_text);
    return 0;
}

static int _ntp_sync(const struct shell *shell, size_t argc, char **argv)
{
    net_time_sync();
    shell_print(shell, "Syncing in the background, the result is logged");
    return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_ntp,
    SHELL_CMD_ARG(status, NULL, "Local time and time zone", _ntp_status, 1, 0),
    SHELL_CMD_ARG(sync, NULL, "Sync now (reads tz / ntp_server from the config)", _ntp_sync, 1, 0),
    SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(ntp, &sub_ntp, "Network time", NULL);
