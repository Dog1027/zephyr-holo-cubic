#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/net_event.h>
#include <zephyr/net/socket.h>
#include <zephyr/shell/shell.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "net_job.h"
#include "sdconf.h"
#include "weather.h"
#include "wlan.h"

LOG_MODULE_REGISTER(weather, CONFIG_WLAN_LOG_LEVEL);

#define HOST            "wttr.in"
#define KEY_LOCATION    "weather_location"
#define FETCH_TIMEOUT_MS 30000
#define UPDATE_DELAY    K_MINUTES(30)
#define RETRY_DELAY     K_SECONDS(60)
// 中午的天氣代表那一天
#define NOON            1200
#define DAY_MAX         BIT(0)
#define DAY_MIN         BIT(1)

/*
 * wttr.in 的 format=j1 有 40 KB（逐時預報），放不進 RAM，所以邊收邊解析：
 * 一個很小的 JSON lexer 只認字串，吐出 (key, value)；值是陣列或物件時吐出 (key, NULL)
 * 當作進入一段。wttr.in 所有的值都是字串，這樣就夠了。
 */

typedef enum { SEC_NONE, SEC_CURRENT, SEC_AREA, SEC_WEATHER } section_t;

typedef struct {
    // lexer
    bool in_str;
    bool escape;
    bool after_colon;       // 上一個 token 是 ':'，下一個字串是值
    bool str_is_value;
    uint8_t len;
    char key[24];
    char str[48];
    // 語意
    section_t section;
    char sub[16];           // 最近進入的陣列 / 物件的 key，例如 "weatherDesc"
    int day;                // weather 陣列裡第幾天，-1 是還沒開始
    int hour;               // hourly 裡目前這筆的 time
    // HTTP
    int hdr_state;          // 已經對到 "\r\n\r\n" 的幾個字
    int hdr_len;
    char status[16];        // 第一行的開頭，例如 "HTTP/1.0 200"
    bool got_current;
    uint8_t day_fields[WEATHER_DAYS];   // 每天收到的欄位，DAY_MAX | DAY_MIN
    size_t body_len;
    weather_t out;
} parser_t;

static void fetch_work_fn(struct k_work *work);

// DRAM region 0 幾乎滿了，這裡的狀態都放 noinit，在 weather_init() 裡初始化
static struct k_work_delayable fetch_work __noinit;
static struct net_mgmt_event_callback l4_cb __noinit;
static parser_t parser __noinit;
static weather_t current __noinit;
static char rx_buf[512] __noinit;
static struct k_spinlock lock;
static bool have_data;
static uint8_t state;
static int16_t last_error;

static weather_kind_t kind_from_code(int code)
{
    // worldweatheronline 的天氣代碼
    switch (code) {
    case 113:
        return WEATHER_SUNNY;
    case 116:
        return WEATHER_PARTLY;
    case 119:
    case 122:
        return WEATHER_CLOUDY;
    case 143:
    case 248:
    case 260:
        return WEATHER_FOG;
    case 200:
    case 386:
    case 389:
    case 392:
    case 395:
        return WEATHER_THUNDER;
    case 179:
    case 182:
    case 185:
    case 227:
    case 230:
    case 317:
    case 320:
    case 323 ... 338:
    case 350:
    case 362 ... 377:
        return WEATHER_SNOW;
    default:
        // 其他都是各種雨
        return WEATHER_RAIN;
    }
}

// Tomohiko Sakamoto 的星期公式，0 = 星期日
static int weekday(int y, int m, int d)
{
    static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};

    if (m < 3) {
        y -= 1;
    }
    return (y + y / 4 - y / 100 + y / 400 + t[(m - 1) % 12] + d) % 7;
}

static void copy_trim(char *dst, size_t size, const char *src)
{
    while (*src == ' ') {
        src++;
    }
    strncpy(dst, src, size - 1);
    dst[size - 1] = '\0';
    for (size_t n = strlen(dst); n > 0 && dst[n - 1] == ' '; n--) {
        dst[n - 1] = '\0';
    }
}

static void on_section(parser_t *p, const char *key)
{
    if (strcmp(key, "current_condition") == 0) {
        p->section = SEC_CURRENT;
    }
    else if (strcmp(key, "nearest_area") == 0) {
        p->section = SEC_AREA;
    }
    else if (strcmp(key, "weather") == 0) {
        p->section = SEC_WEATHER;
        p->day = -1;
    }
    else if (strcmp(key, "request") == 0) {
        p->section = SEC_NONE;
    }
    copy_trim(p->sub, sizeof(p->sub), key);
}

static void on_pair(parser_t *p, const char *key, const char *value)
{
    weather_t *w = &p->out;

    switch (p->section) {
    case SEC_CURRENT:
        p->got_current = true;
        if (strcmp(key, "temp_C") == 0) {
            w->temp_c = atoi(value);
        }
        else if (strcmp(key, "FeelsLikeC") == 0) {
            w->feels_c = atoi(value);
        }
        else if (strcmp(key, "humidity") == 0) {
            w->humidity = atoi(value);
        }
        else if (strcmp(key, "windspeedKmph") == 0) {
            w->wind_kmph = atoi(value);
        }
        else if (strcmp(key, "winddir16Point") == 0) {
            copy_trim(w->wind_dir, sizeof(w->wind_dir), value);
        }
        else if (strcmp(key, "weatherCode") == 0) {
            w->kind = kind_from_code(atoi(value));
        }
        else if (strcmp(key, "value") == 0 && strcmp(p->sub, "weatherDesc") == 0) {
            copy_trim(w->desc, sizeof(w->desc), value);
        }
        break;

    case SEC_AREA:
        if (strcmp(key, "value") == 0 && strcmp(p->sub, "areaName") == 0) {
            copy_trim(w->area, sizeof(w->area), value);
        }
        break;

    case SEC_WEATHER: {
        if (strcmp(key, "date") == 0) {
            p->day++;
            if (p->day < WEATHER_DAYS) {
                int y = 0, m = 0, d = 0;
                sscanf(value, "%d-%d-%d", &y, &m, &d);
                memset(&w->days[p->day], 0, sizeof(w->days[0]));
                w->days[p->day].wday = weekday(y, m, d);
                w->days[p->day].kind = WEATHER_CLOUDY;
                w->day_count = p->day + 1;
            }
            break;
        }
        if (p->day < 0 || p->day >= WEATHER_DAYS) {
            break;
        }
        weather_day_t *day = &w->days[p->day];
        if (strcmp(key, "maxtempC") == 0) {
            day->max_c = atoi(value);
            p->day_fields[p->day] |= DAY_MAX;
        }
        else if (strcmp(key, "mintempC") == 0) {
            day->min_c = atoi(value);
            p->day_fields[p->day] |= DAY_MIN;
        }
        else if (strcmp(key, "time") == 0) {
            p->hour = atoi(value);
        }
        else if (strcmp(key, "chanceofrain") == 0) {
            day->rain = MAX(day->rain, atoi(value));
        }
        // hourly 裡 time 排在 weatherCode 前面
        else if (strcmp(key, "weatherCode") == 0 && p->hour == NOON) {
            day->kind = kind_from_code(atoi(value));
        }
        break;
    }

    default:
        break;
    }
}

static void lex(parser_t *p, char c)
{
    if (p->in_str) {
        if (p->escape) {
            p->escape = false;
        }
        else if (c == '\\') {
            // 跳脫字元只保留後面那個字（wttr.in 的英文描述用不到 \u）
            p->escape = true;
            return;
        }
        else if (c == '"') {
            p->in_str = false;
            p->str[p->len] = '\0';
            if (p->str_is_value) {
                on_pair(p, p->key, p->str);
            }
            else {
                copy_trim(p->key, sizeof(p->key), p->str);
            }
            p->after_colon = false;
            return;
        }
        if (p->len < sizeof(p->str) - 1) {
            p->str[p->len++] = c;
        }
        return;
    }

    switch (c) {
    case '"':
        p->in_str = true;
        p->str_is_value = p->after_colon;
        p->len = 0;
        break;
    case ':':
        p->after_colon = true;
        break;
    case '[':
    case '{':
        if (p->after_colon) {
            on_section(p, p->key);
        }
        p->after_colon = false;
        break;
    case ',':
        p->after_colon = false;
        break;
    default:
        break;
    }
}

// 先吃掉 HTTP header（記下狀態列），之後的內容交給 lexer
static void feed(parser_t *p, const char *data, size_t len)
{
    static const char hdr_end[] = "\r\n\r\n";

    for (size_t i = 0; i < len; i++) {
        char c = data[i];
        if (p->hdr_state < 4) {
            if (p->hdr_len < sizeof(p->status) - 1) {
                p->status[p->hdr_len++] = c;
                p->status[p->hdr_len] = '\0';
            }
            p->hdr_state = (c == hdr_end[p->hdr_state]) ? p->hdr_state + 1 : (c == '\r');
            continue;
        }
        p->body_len++;
        lex(p, c);
    }
}

// 地點放進 URL path：英數字和幾個符號照用，空白變 '+'，其他用 %XX
static void url_encode(char *dst, size_t size, const char *src)
{
    size_t n = 0;

    for (; *src && n + 4 < size; src++) {
        unsigned char c = *src;
        if (isalnum(c) || strchr("-_.,~@", c)) {
            dst[n++] = c;
        }
        else if (c == ' ') {
            dst[n++] = '+';
        }
        else {
            n += snprintf(dst + n, size - n, "%%%02X", c);
        }
    }
    dst[n] = '\0';
}

static bool days_complete(const parser_t *p)
{
    if (p->out.day_count != WEATHER_DAYS) {
        return false;
    }
    for (int i = 0; i < WEATHER_DAYS; i++) {
        if (p->day_fields[i] != (DAY_MAX | DAY_MIN)) {
            return false;
        }
    }
    return true;
}

static int send_all(int fd, const char *buf, size_t len)
{
    while (len > 0) {
        ssize_t sent = zsock_send(fd, buf, len, 0);
        if (sent < 0) {
            return -errno;
        }
        buf += sent;
        len -= sent;
    }
    return 0;
}

static int fetch(void)
{
    char location[48];
    char path[96];
    char request[192];
    struct zsock_addrinfo hints = {
        .ai_family = NET_AF_INET,
        .ai_socktype = NET_SOCK_STREAM,
    };
    struct zsock_addrinfo *addr;
    int64_t start = k_uptime_get();
    int64_t deadline = start + FETCH_TIMEOUT_MS;
    int fd;
    int ret;

    if (sdconf_get(KEY_LOCATION, location, sizeof(location)) <= 0) {
        location[0] = '\0';     // wttr.in 用來源 IP 判斷地點
    }
    url_encode(path, sizeof(path), location);
    snprintf(request, sizeof(request),
             "GET /%s?format=j1&m HTTP/1.0\r\nHost: " HOST "\r\nUser-Agent: holocubic\r\n\r\n", path);

    ret = zsock_getaddrinfo(HOST, "80", &hints, &addr);
    if (ret != 0) {
        LOG_WRN("DNS lookup of " HOST " failed (%d)", ret);
        return -EHOSTUNREACH;
    }
    fd = zsock_socket(addr->ai_family, addr->ai_socktype, NET_IPPROTO_TCP);
    if (fd < 0) {
        zsock_freeaddrinfo(addr);
        return -errno;
    }
    ret = zsock_connect(fd, addr->ai_addr, addr->ai_addrlen);
    zsock_freeaddrinfo(addr);
    if (ret < 0) {
        ret = -errno;
        goto out;
    }
    ret = send_all(fd, request, strlen(request));
    if (ret < 0) {
        goto out;
    }

    memset(&parser, 0, sizeof(parser));
    parser.day = -1;
    parser.hour = -1;

    // HTTP/1.0：沒有 Content-Length，讀到對方關閉連線為止
    while (true) {
        struct zsock_pollfd pfd = {.fd = fd, .events = ZSOCK_POLLIN};
        int64_t left = deadline - k_uptime_get();
        if (left <= 0) {
            ret = -ETIMEDOUT;
            goto out;
        }
        ret = zsock_poll(&pfd, 1, (int)left);
        if (ret < 0) {
            ret = -errno;
            goto out;
        }
        if (ret == 0) {
            ret = -ETIMEDOUT;
            goto out;
        }
        ssize_t n = zsock_recv(fd, rx_buf, sizeof(rx_buf), 0);
        if (n < 0) {
            ret = -errno;
            goto out;
        }
        if (n == 0) {
            break;
        }
        feed(&parser, rx_buf, n);
    }

    if (strncmp(parser.status, "HTTP/1.", 7) != 0 || atoi(parser.status + 9) != 200) {
        LOG_WRN("Unexpected reply: %s", parser.status);
        ret = -EPROTO;
    }
    else if (!parser.got_current || !days_complete(&parser)) {
        // 連線提早結束時資料會不完整，例如最後一天沒有最高 / 最低溫
        LOG_WRN("Incomplete reply (%zu bytes)", parser.body_len);
        ret = -EBADMSG;
    }
    else {
        LOG_DBG("%zu bytes in %lld ms", parser.body_len, k_uptime_get() - start);
        ret = 0;
    }

out:
    zsock_close(fd);
    return ret;
}

static void set_state(weather_state_t s)
{
    k_spinlock_key_t key = k_spin_lock(&lock);
    state = s;
    k_spin_unlock(&lock, key);
}

static void fetch_work_fn(struct k_work *work)
{
    if (!wlan_is_online()) {
        set_state(WEATHER_OFFLINE);
        return;
    }
    set_state(WEATHER_LOADING);

    int ret = fetch();
    k_spinlock_key_t key = k_spin_lock(&lock);
    if (ret == 0) {
        parser.out.generation = current.generation + 1;
        parser.out.updated_ms = k_uptime_get();
        current = parser.out;
        have_data = true;
        state = WEATHER_READY;
    }
    else {
        last_error = ret;
        state = WEATHER_FAILED;
    }
    k_spin_unlock(&lock, key);

    if (ret == 0) {
        LOG_INF("Weather for %s: %d C, %s", current.area, current.temp_c, current.desc);
        k_work_reschedule_for_queue(net_job_queue(), &fetch_work, UPDATE_DELAY);
    }
    else {
        LOG_WRN("Weather update failed (%d), retrying in 60 s", ret);
        k_work_reschedule_for_queue(net_job_queue(), &fetch_work, RETRY_DELAY);
    }
}

bool weather_get(weather_t *out)
{
    k_spinlock_key_t key = k_spin_lock(&lock);
    bool ok = have_data;
    if (ok) {
        *out = current;
    }
    k_spin_unlock(&lock, key);
    return ok;
}

weather_state_t weather_state(void)
{
    if (!wlan_is_online()) {
        return WEATHER_OFFLINE;
    }
    // 剛上線、還沒輪到抓的時候也算載入中
    return state == WEATHER_OFFLINE ? WEATHER_LOADING : state;
}

int weather_error(void)
{
    return last_error;
}

void weather_update(void)
{
    k_work_reschedule_for_queue(net_job_queue(), &fetch_work, K_NO_WAIT);
}

// 上線後等一下再抓，讓 SNTP 先跑（兩個都在 net_job queue 上，依序執行）
static void l4_event_handler(struct net_mgmt_event_callback *cb, uint64_t event,
                             struct net_if *iface)
{
    k_work_reschedule_for_queue(net_job_queue(), &fetch_work, K_SECONDS(1));
}

static int weather_init(void)
{
    memset(&current, 0, sizeof(current));
    k_work_init_delayable(&fetch_work, fetch_work_fn);
    net_mgmt_init_event_callback(&l4_cb, l4_event_handler, NET_EVENT_L4_CONNECTED);
    net_mgmt_add_event_callback(&l4_cb);
    return 0;
}

SYS_INIT(weather_init, APPLICATION, CONFIG_WLAN_INIT_PRIORITY);

/* ---------- shell ---------- */

static const char *const kind_names[] = {"sunny", "partly cloudy", "cloudy", "fog",
                                         "rain", "snow", "thunder"};
static const char *const wday_names[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};

static int _weather_show(const struct shell *shell, size_t argc, char **argv)
{
    static const char *const state_names[] = {"waiting for Wi-Fi", "loading", "ready", "failed"};
    weather_state_t s = weather_state();
    weather_t data;
    weather_t *w = &data;

    if (s == WEATHER_FAILED) {
        shell_print(shell, "State: failed (%d)", weather_error());
    }
    else {
        shell_print(shell, "State: %s", state_names[s]);
    }
    if (!weather_get(w)) {
        return 0;
    }
    shell_print(shell, "%s, updated %lld min ago", w->area,
                (k_uptime_get() - w->updated_ms) / 60000);
    shell_print(shell, "Now: %d C (feels %d C), %s [%s], humidity %d%%, wind %d km/h %s",
                w->temp_c, w->feels_c, w->desc, kind_names[w->kind], w->humidity, w->wind_kmph,
                w->wind_dir);
    for (int i = 0; i < w->day_count; i++) {
        const weather_day_t *d = &w->days[i];
        shell_print(shell, "%s: %d / %d C, %s, rain %d%%", wday_names[d->wday], d->max_c, d->min_c,
                    kind_names[d->kind], d->rain);
    }
    return 0;
}

static int _weather_update(const struct shell *shell, size_t argc, char **argv)
{
    if (!wlan_is_online()) {
        shell_error(shell, "Wi-Fi is not online");
        return -ENETDOWN;
    }
    weather_update();
    shell_print(shell, "Updating in the background, the result is logged");
    return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_weather,
    SHELL_CMD_ARG(update, NULL, "Update now (reads weather_location from the config)",
                  _weather_update, 1, 0),
    SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(weather, &sub_weather, "Weather from wttr.in", _weather_show);
