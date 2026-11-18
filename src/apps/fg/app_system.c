#include "fg_app.h"
#include "file.h"
#include "ui.h"
#include "ui_tabs.h"
#include "ui_wheel.h"
#include "input.h"
#include "wlan.h"
#include <zephyr/kernel.h>
#include <zephyr/fs/fs.h>
#include <zephyr/net/wifi.h>
#include <zephyr/sys/sys_heap.h>
#include <zephyr/version.h>
#include <lvgl_mem.h>
#include <string.h>
#include <stdio.h>

/*
 * 系統狀態，兩個分頁（ui_tabs.h）：
 *   SYSTEM：開機時間、heap、SD 卡、姿態、每個 thread 的 stack 用量、板子和 Zephyr 版本
 *   WI-FI：連線狀態、訊號強度（含一分鐘的曲線）、位址、附近的網路（切到這頁時掃描）
 * 一次顯示一項（ui_wheel.h），數值用大字：上下換項目，OK 或敲一下看下一項。
 * 切換分頁時整頁重建。
 */

#define SYSTEM_ACCENT_COLOR     THEME_PURPLE
#define SYS_REFRESH_MS          500
#define SYS_SD_REFRESH_MS       5000
#define SYS_SD_PATH             FILE_ROOT
#define SYS_MAX_THREADS         16

#define NET_ACCENT_COLOR        THEME_BLUE
#define NET_REFRESH_MS          1000
#define NET_CHART_POINTS        60      // 每秒一點，一分鐘
#define NET_CHART_WIDTH         200
#define NET_CHART_HEIGHT        40
#define NET_CHART_Y             66      // 在 ui_wheel 中間那一項裡的位置，數值下面
#define NET_RSSI_MIN            (-100)
#define NET_RSSI_MAX            (-20)

extern struct k_heap _system_heap;

enum { MODE_SYSTEM, MODE_WIFI, MODE_COUNT };

// SYSTEM 的項目：固定的幾項，接著每個 thread 一項，最後是板子和版本
enum {
    SYS_UPTIME,
    SYS_SYS_HEAP,
    SYS_LVGL_HEAP,
    SYS_SD,
    SYS_ROLL,
    SYS_PITCH,
    SYS_FIXED_COUNT,
};
#define SYS_TAIL_COUNT  2   // Board, Zephyr

// WI-FI 的項目：固定的幾項，接著每個掃到的網路一項
enum {
    NET_STATUS,
    NET_SSID,
    NET_SIGNAL,
    NET_CHANNEL,
    NET_IP,
    NET_NETMASK,
    NET_GATEWAY,
    NET_MAC,
    NET_NEARBY,
    NET_FIXED_COUNT,
};

typedef struct {
    k_tid_t threads[SYS_MAX_THREADS];
    int thread_count;
    uint32_t sd_used_mb;
    uint32_t sd_total_mb;   // 0 表示沒有卡
    int64_t last_sd_refresh;
} sys_page_t;

typedef struct {
    wlan_info_t info;
    wlan_ap_t aps[WLAN_SCAN_MAX];
    int ap_count;
    bool scanning;
    int scan_error;
    lv_obj_t *chart;
    lv_chart_series_t *series;
} net_page_t;

typedef struct {
    lv_obj_t *scr;
    lv_obj_t *page;         // 目前分頁的容器，切換時整個刪掉
    ui_wheel_t wheel;
    int mode;
    int64_t last_refresh;
    union {                 // 只有目前那一頁的狀態，切換時清掉
        sys_page_t sys;
        net_page_t net;
    } state;
} self_t;

APP_FG_REGISTER(System, ICON_SYSTEM, app_system_init, app_system_run, app_system_exit, 13, self_t);

static int percent_of(uint64_t part, uint64_t total)
{
    return total > 0 ? (int)(part * 100 / total) : 0;
}

/* ---------- SYSTEM ---------- */

// 角度轉成一位小數的字串，不用 %f (picolibc 可能沒有浮點 printf)
static void angle_format(char *buf, size_t len, float deg)
{
    int tenths = (int)(deg * 10.0f + (deg < 0 ? -0.5f : 0.5f));
    const char *sign = tenths < 0 ? "-" : "";

    if (tenths < 0) {
        tenths = -tenths;
    }
    snprintf(buf, len, "%s%d.%d°", sign, tenths / 10, tenths % 10);
}

static void heap_item(ui_wheel_item_t *item, const struct sys_memory_stats *stats)
{
    size_t total = stats->allocated_bytes + stats->free_bytes;

    snprintf(item->value, sizeof(item->value), "%u / %u KB", (unsigned int)((stats->allocated_bytes + 1023) / 1024),
             (unsigned int)(total / 1024));
    item->percent = percent_of(stats->allocated_bytes, total);
}

static void uptime_format(char *buf, size_t len)
{
    uint32_t sec = (uint32_t)(k_uptime_get() / 1000);
    uint32_t days = sec / 86400;

    sec %= 86400;
    if (days > 0) {
        snprintf(buf, len, "%ud %02u:%02u:%02u", (unsigned int)days, (unsigned int)(sec / 3600),
                 (unsigned int)(sec / 60 % 60), (unsigned int)(sec % 60));
    }
    else {
        snprintf(buf, len, "%u:%02u:%02u", (unsigned int)(sec / 3600), (unsigned int)(sec / 60 % 60),
                 (unsigned int)(sec % 60));
    }
}

static void thread_item(k_tid_t thread, ui_wheel_item_t *item, bool name_only)
{
    const char *name = k_thread_name_get(thread);
    size_t unused;

    if (name != NULL && name[0] != '\0') {
        snprintf(item->name, sizeof(item->name), "%s", name);
    }
    else {
        snprintf(item->name, sizeof(item->name), "%p", (void *)thread);
    }
    if (name_only) {
        return;
    }
    // 執行緒結束後 stack 可能已無效，查詢失敗就顯示 "-"
    if (k_thread_stack_space_get(thread, &unused) != 0) {
        snprintf(item->value, sizeof(item->value), "-");
        return;
    }
    size_t size = thread->stack_info.size;
    snprintf(item->value, sizeof(item->value), "%u / %u", (unsigned int)(size - unused), (unsigned int)size);
    item->percent = percent_of(size - unused, size);
}

static void sys_get(int index, ui_wheel_item_t *item, bool name_only)
{
    sys_page_t *p = &self->state.sys;
    struct sys_memory_stats stats;
    input_angle_t angle;
    static const char *const names[SYS_FIXED_COUNT] = {
        "Uptime", "Sys heap", "LVGL heap", "SD card", "Roll", "Pitch",
    };

    if (index >= SYS_FIXED_COUNT && index < SYS_FIXED_COUNT + p->thread_count) {
        thread_item(p->threads[index - SYS_FIXED_COUNT], item, name_only);
        return;
    }
    if (index >= SYS_FIXED_COUNT) {
        bool board = index == SYS_FIXED_COUNT + p->thread_count;
        snprintf(item->name, sizeof(item->name), "%s", board ? "Board" : "Zephyr");
        snprintf(item->value, sizeof(item->value), "%s", board ? CONFIG_BOARD : KERNEL_VERSION_STRING);
        return;
    }

    snprintf(item->name, sizeof(item->name), "%s", names[index]);
    if (name_only) {
        return;
    }
    switch (index) {
    case SYS_UPTIME:
        uptime_format(item->value, sizeof(item->value));
        break;
    case SYS_SYS_HEAP:
        sys_heap_runtime_stats_get(&_system_heap.heap, &stats);
        heap_item(item, &stats);
        break;
    case SYS_LVGL_HEAP:
        lvgl_heap_stats(&stats);
        heap_item(item, &stats);
        break;
    case SYS_SD:
        if (p->sd_total_mb == 0) {
            snprintf(item->value, sizeof(item->value), "No card");
            break;
        }
        snprintf(item->value, sizeof(item->value), "%u / %u MB", (unsigned int)p->sd_used_mb,
                 (unsigned int)p->sd_total_mb);
        item->percent = percent_of(p->sd_used_mb, p->sd_total_mb);
        break;
    default:
        input_get(&angle);
        angle_format(item->value, sizeof(item->value), index == SYS_ROLL ? angle.roll : angle.pitch);
        break;
    }
}

static void thread_collect_cb(const struct k_thread *thread, void *user_data)
{
    sys_page_t *p = &self->state.sys;

    if (p->thread_count < SYS_MAX_THREADS) {
        p->threads[p->thread_count++] = (k_tid_t)thread;
    }
}

// SD 卡查一次要讀 FAT，不用每次刷新都查
static void sys_update_sd(int64_t now)
{
    sys_page_t *p = &self->state.sys;
    struct fs_statvfs st;

    if (p->last_sd_refresh != 0 && now - p->last_sd_refresh < SYS_SD_REFRESH_MS) {
        return;
    }
    p->last_sd_refresh = now;
    if (fs_statvfs(SYS_SD_PATH, &st) < 0 || st.f_blocks == 0) {
        p->sd_total_mb = 0;
        return;
    }
    p->sd_total_mb = (uint32_t)(((uint64_t)st.f_frsize * st.f_blocks) >> 20);
    p->sd_used_mb = (uint32_t)(((uint64_t)st.f_frsize * (st.f_blocks - st.f_bfree)) >> 20);
}

static void sys_create(void)
{
    sys_page_t *p = &self->state.sys;

    k_thread_foreach_unlocked(thread_collect_cb, NULL);
    sys_update_sd(k_uptime_get());
    ui_wheel_create(&self->wheel, self->page, UI_TABS_BOTTOM, SYSTEM_ACCENT_COLOR, sys_get,
                    SYS_FIXED_COUNT + p->thread_count + SYS_TAIL_COUNT);
}

/* ---------- WI-FI ---------- */

static const char *net_status_text(const wlan_info_t *info)
{
    if (info->online) {
        return "Online";
    }
    if (info->state == WIFI_STATE_COMPLETED) {
        return "No IP yet";
    }
    if (info->state >= WIFI_STATE_SCANNING && info->state < WIFI_STATE_COMPLETED) {
        return "Connecting";
    }
    return "Offline";
}

static int rssi_percent(int rssi)
{
    return (CLAMP(rssi, NET_RSSI_MIN, NET_RSSI_MAX) - NET_RSSI_MIN) * 100 / (NET_RSSI_MAX - NET_RSSI_MIN);
}

static void net_get(int index, ui_wheel_item_t *item, bool name_only)
{
    net_page_t *p = &self->state.net;
    const wlan_info_t *info = &p->info;
    bool linked = info->state == WIFI_STATE_COMPLETED;
    static const char *const names[NET_FIXED_COUNT] = {
        "Status", "SSID", "Signal", "Channel", "IP", "Netmask", "Gateway", "MAC", "Nearby networks",
    };

    if (index >= NET_FIXED_COUNT) {
        const wlan_ap_t *ap = &p->aps[index - NET_FIXED_COUNT];
        snprintf(item->name, sizeof(item->name), "%s", ap->ssid[0] ? ap->ssid : "(hidden)");
        snprintf(item->value, sizeof(item->value), "%d dBm", ap->rssi);
        item->percent = rssi_percent(ap->rssi);
        return;
    }

    snprintf(item->name, sizeof(item->name), "%s", names[index]);
    if (name_only) {
        return;
    }
    switch (index) {
    case NET_STATUS:
        snprintf(item->value, sizeof(item->value), "%s", net_status_text(info));
        break;
    case NET_SSID:
        snprintf(item->value, sizeof(item->value), "%s", linked ? info->ssid : "-");
        break;
    case NET_SIGNAL:
        if (linked) {
            snprintf(item->value, sizeof(item->value), "%d dBm", info->rssi);
        }
        else {
            snprintf(item->value, sizeof(item->value), "-");
        }
        item->extra = p->chart;
        break;
    case NET_CHANNEL:
        if (linked) {
            snprintf(item->value, sizeof(item->value), "%u", info->channel);
        }
        else {
            snprintf(item->value, sizeof(item->value), "-");
        }
        break;
    case NET_IP:
        snprintf(item->value, sizeof(item->value), "%s", info->ip[0] ? info->ip : "-");
        break;
    case NET_NETMASK:
        snprintf(item->value, sizeof(item->value), "%s", info->netmask[0] ? info->netmask : "-");
        break;
    case NET_GATEWAY:
        snprintf(item->value, sizeof(item->value), "%s", info->gateway[0] ? info->gateway : "-");
        break;
    case NET_MAC:
        snprintf(item->value, sizeof(item->value), "%02x:%02x:%02x:%02x:%02x:%02x", info->mac[0], info->mac[1],
                 info->mac[2], info->mac[3], info->mac[4], info->mac[5]);
        break;
    default:
        if (p->scanning) {
            snprintf(item->value, sizeof(item->value), "Scanning");
        }
        else if (p->scan_error != 0) {
            snprintf(item->value, sizeof(item->value), "Failed (%d)", p->scan_error);
        }
        else {
            snprintf(item->value, sizeof(item->value), "%d found", p->ap_count);
        }
        break;
    }
}

// 訊號曲線：Signal 那一項的內容，不在那一項時藏起來，但每秒都記
static lv_obj_t *net_chart_create(lv_obj_t *parent)
{
    net_page_t *p = &self->state.net;
    lv_obj_t *obj = lv_chart_create(parent);

    lv_obj_set_size(obj, NET_CHART_WIDTH, NET_CHART_HEIGHT);
    lv_obj_align(obj, LV_ALIGN_TOP_MID, 0, NET_CHART_Y);
    lv_obj_set_clickable(obj, false);
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 2, 0);
    lv_obj_set_style_line_color(obj, lv_color_hex(THEME_TEXT_DIM), LV_PART_MAIN);
    lv_obj_set_style_line_opa(obj, LV_OPA_30, LV_PART_MAIN);
    lv_obj_set_style_line_width(obj, 2, LV_PART_ITEMS);
    // 不畫每個點的圓點，只畫線
    lv_obj_set_style_size(obj, 0, 0, LV_PART_INDICATOR);

    lv_chart_set_type(obj, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(obj, NET_CHART_POINTS);
    lv_chart_set_update_mode(obj, LV_CHART_UPDATE_MODE_SHIFT);
    lv_chart_set_axis_range(obj, LV_CHART_AXIS_PRIMARY_Y, NET_RSSI_MIN, NET_RSSI_MAX);
    // 水平格線每 20 dBm 一條
    lv_chart_set_div_line_count(obj, (NET_RSSI_MAX - NET_RSSI_MIN) / 20 - 1, 0);
    p->series = lv_chart_add_series(obj, lv_color_hex(NET_ACCENT_COLOR), LV_CHART_AXIS_PRIMARY_Y);
    lv_chart_set_all_values(obj, p->series, LV_CHART_POINT_NONE);
    lv_obj_set_hidden(obj, true);
    return obj;
}

static void net_update(void)
{
    net_page_t *p = &self->state.net;

    wlan_info(&p->info);
    // 沒連上時畫空點，曲線會斷開
    bool linked = p->info.state == WIFI_STATE_COMPLETED;
    int32_t point = linked ? CLAMP(p->info.rssi, NET_RSSI_MIN, NET_RSSI_MAX) : LV_CHART_POINT_NONE;
    lv_chart_set_next_value(p->chart, p->series, point);
}

static void net_create(void)
{
    net_page_t *p = &self->state.net;

    wlan_info(&p->info);
    int ret = wlan_scan();
    p->scanning = ret == 0 || ret == -EBUSY;
    p->scan_error = p->scanning ? 0 : ret;

    ui_wheel_create(&self->wheel, self->page, UI_TABS_BOTTOM, NET_ACCENT_COLOR, net_get, NET_FIXED_COUNT);
    p->chart = net_chart_create(self->wheel.box);
}

static void net_run(void)
{
    net_page_t *p = &self->state.net;

    if (p->scanning && !wlan_scan_busy()) {
        p->scanning = false;
        p->ap_count = wlan_scan_get(p->aps, ARRAY_SIZE(p->aps));
        ui_wheel_set_count(&self->wheel, NET_FIXED_COUNT + p->ap_count);
    }
}

/* ---------- 分頁 ---------- */

static void refresh(int64_t now)
{
    self->last_refresh = now;
    if (self->mode == MODE_SYSTEM) {
        sys_update_sd(now);
    }
    else {
        net_update();
    }
    ui_wheel_refresh(&self->wheel);
}

static lv_obj_t *page_create(lv_obj_t *scr)
{
    lv_obj_t *page = lv_obj_create(scr);
    lv_obj_remove_style_all(page);
    lv_obj_set_size(page, LV_PCT(100), LV_PCT(100));
    lv_obj_set_scrollable(page, false);
    lv_obj_set_clickable(page, false);
    return page;
}

// 刪掉目前那一頁，建立 mode 那一頁
static void tab_show(int mode)
{
    self->mode = mode;
    if (self->page != NULL) {
        lv_obj_delete(self->page);
    }
    memset(&self->state, 0, sizeof(self->state));
    memset(&self->wheel, 0, sizeof(self->wheel));

    self->page = page_create(self->scr);
    // 分頁籤要畫在上面，頁面移到最底層
    lv_obj_move_to_index(self->page, 0);
    if (mode == MODE_SYSTEM) {
        sys_create();
    }
    else {
        net_create();
    }
    self->last_refresh = k_uptime_get();
}

static void tab_action(int mode)
{
    ui_wheel_move(&self->wheel, 1);
}

static void tab_scroll(int mode, int dir)
{
    ui_wheel_move(&self->wheel, dir);
}

static void app_system_init(void *scr)
{
    ui_screen_init(scr);
    self->scr = scr;

    static const char *const names[] = {"SYSTEM", "WI-FI"};
    static const ui_tabs_cfg_t tabs = {
        .names = names, .count = ARRAY_SIZE(names), .accent = SYSTEM_ACCENT_COLOR,
        .pref_key = "system/mode", .show = tab_show, .action = tab_action, .scroll = tab_scroll,
    };
    ui_tabs_create(scr, &tabs);
}

static void app_system_run(void *scr)
{
    int64_t now = k_uptime_get();

    if (self->mode == MODE_WIFI) {
        net_run();
    }
    if (now - self->last_refresh >= (self->mode == MODE_SYSTEM ? SYS_REFRESH_MS : NET_REFRESH_MS)) {
        refresh(now);
    }
}

static void app_system_exit(void *scr)
{
}
