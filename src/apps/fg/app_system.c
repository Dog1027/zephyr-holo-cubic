#include "fg_app.h"
#include "file.h"
#include "ui.h"
#include "nav.h"
#include "input.h"
#include "prefs.h"
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
 * 系統狀態，兩個分頁：
 *   SYSTEM：開機時間、heap、SD 卡、姿態、各 thread 的 stack 用量
 *   NETWORK：Wi-Fi 狀態、訊號強度曲線、掃描附近的網路
 * 兩頁都是清單，next / prev 在清單裡移動，所以敲一下才是切換分頁（分頁籤只顯示）。
 * 只建立目前那一頁：每一列都佔 LVGL 記憶體，兩頁一起放不下，切換時整頁重建。
 */

#define SYSTEM_ACCENT_COLOR     THEME_PURPLE
#define SYS_REFRESH_MS          500
#define SYS_SD_REFRESH_MS       5000
#define SYS_SD_PATH             FILE_ROOT
#define SYS_MAX_THREADS         12  // 每列都佔 LVGL 記憶體，總列數控制在 ~20

#define NET_ACCENT_COLOR        THEME_BLUE
#define NET_REFRESH_MS          1000
#define NET_CHART_POINTS        60      // 每秒一點，一分鐘
#define NET_CHART_HEIGHT        56
#define NET_RSSI_MIN            (-100)
#define NET_RSSI_MAX            (-20)

extern struct k_heap _system_heap;

enum { MODE_SYSTEM, MODE_NETWORK, MODE_COUNT };

enum {
    SYS_ROW_UPTIME,
    SYS_ROW_SYS_HEAP,
    SYS_ROW_LVGL_HEAP,
    SYS_ROW_SD,
    SYS_ROW_ROLL,
    SYS_ROW_PITCH,
    SYS_ROW_COUNT,
};

enum {
    NET_ROW_STATUS,
    NET_ROW_SSID,
    NET_ROW_SIGNAL,
    NET_ROW_CHANNEL,
    NET_ROW_IP,
    NET_ROW_NETMASK,
    NET_ROW_GATEWAY,
    NET_ROW_MAC,
    NET_ROW_COUNT,
};

typedef struct {
    lv_obj_t *rows[SYS_ROW_COUNT];          // 每一列右邊的值
    lv_obj_t *thread_rows[SYS_MAX_THREADS];
    k_tid_t threads[SYS_MAX_THREADS];
    int thread_count;
    int64_t last_sd_refresh;
} sys_page_t;

typedef struct {
    lv_obj_t *rows[NET_ROW_COUNT];          // 每一列右邊的值
    lv_obj_t *scan_rows[WLAN_SCAN_MAX];
    int scan_row_count;
    lv_obj_t *chart;
    lv_chart_series_t *series;
    lv_obj_t *scan_detail;
    bool pending_scan;
    bool scanning;
} net_page_t;

typedef struct {
    lv_obj_t *scr;
    lv_obj_t *tabs;
    lv_obj_t *list;
    int mode;
    int saved_mode;
    bool pending_switch;
    int64_t last_refresh;
    union {                 // 只有目前那一頁的狀態，切換時清掉
        sys_page_t sys;
        net_page_t net;
    } page;
} self_t;

APP_FG_REGISTER(System, ICON_SYSTEM, app_system_init, app_system_run, app_system_exit, 13, self_t);

static lv_obj_t *row_create(const char *icon, const char *name, uint32_t accent)
{
    return ui_list_row(self->list, icon, name, lv_color_hex(accent));
}

/* ---------- SYSTEM ---------- */

// 角度轉成一位小數的字串，不用 %f (picolibc 可能沒有浮點 printf)
static void row_set_angle(lv_obj_t *row, float deg)
{
    int tenths = (int)(deg * 10.0f + (deg < 0 ? -0.5f : 0.5f));
    const char *sign = tenths < 0 ? "-" : "";

    if (tenths < 0) {
        tenths = -tenths;
    }
    ui_label_set_fmt(row, "%s%d.%d deg", sign, tenths / 10, tenths % 10);
}

static void thread_collect_cb(const struct k_thread *thread, void *user_data)
{
    sys_page_t *p = &self->page.sys;

    if (p->thread_count < SYS_MAX_THREADS) {
        p->threads[p->thread_count++] = (k_tid_t)thread;
    }
}

static void sys_update_uptime(lv_obj_t *row)
{
    uint32_t sec = (uint32_t)(k_uptime_get() / 1000);
    uint32_t days = sec / 86400;

    sec %= 86400;
    if (days > 0) {
        ui_label_set_fmt(row, "%ud %02u:%02u:%02u", (unsigned int)days, (unsigned int)(sec / 3600),
                         (unsigned int)(sec / 60 % 60), (unsigned int)(sec % 60));
    }
    else {
        ui_label_set_fmt(row, "%u:%02u:%02u", (unsigned int)(sec / 3600), (unsigned int)(sec / 60 % 60),
                         (unsigned int)(sec % 60));
    }
}

static void sys_update_heap(lv_obj_t *row, const struct sys_memory_stats *stats)
{
    size_t total = stats->allocated_bytes + stats->free_bytes;

    ui_label_set_fmt(row, "%u / %u KB", (unsigned int)((stats->allocated_bytes + 1023) / 1024),
                     (unsigned int)(total / 1024));
}

static void sys_update_sd(lv_obj_t *row)
{
    struct fs_statvfs st;

    if (fs_statvfs(SYS_SD_PATH, &st) < 0 || st.f_blocks == 0) {
        ui_label_set_fmt(row, "No card");
        return;
    }

    uint64_t total = (uint64_t)st.f_frsize * st.f_blocks;
    uint64_t used = (uint64_t)st.f_frsize * (st.f_blocks - st.f_bfree);
    ui_label_set_fmt(row, "%u / %u MB", (unsigned int)(used >> 20), (unsigned int)(total >> 20));
}

static void sys_update_threads(void)
{
    sys_page_t *p = &self->page.sys;

    for (int i = 0; i < p->thread_count; i++) {
        size_t unused;

        // 執行緒結束後 stack 可能已無效，查詢失敗就顯示 "-"
        if (k_thread_stack_space_get(p->threads[i], &unused) != 0) {
            ui_label_set_fmt(p->thread_rows[i], "-");
            continue;
        }

        size_t size = p->threads[i]->stack_info.size;
        ui_label_set_fmt(p->thread_rows[i], "%u / %u", (unsigned int)(size - unused), (unsigned int)size);
    }
}

static void sys_refresh(int64_t now)
{
    sys_page_t *p = &self->page.sys;
    struct sys_memory_stats stats;
    input_angle_t angle;

    sys_update_uptime(p->rows[SYS_ROW_UPTIME]);

    sys_heap_runtime_stats_get(&_system_heap.heap, &stats);
    sys_update_heap(p->rows[SYS_ROW_SYS_HEAP], &stats);

    lvgl_heap_stats(&stats);
    sys_update_heap(p->rows[SYS_ROW_LVGL_HEAP], &stats);

    if (p->last_sd_refresh == 0 || now - p->last_sd_refresh >= SYS_SD_REFRESH_MS) {
        p->last_sd_refresh = now;
        sys_update_sd(p->rows[SYS_ROW_SD]);
    }

    input_get(&angle);
    row_set_angle(p->rows[SYS_ROW_ROLL], angle.roll);
    row_set_angle(p->rows[SYS_ROW_PITCH], angle.pitch);

    sys_update_threads();
}

static void sys_create(void)
{
    sys_page_t *p = &self->page.sys;

    p->rows[SYS_ROW_UPTIME] = row_create(LV_SYMBOL_REFRESH, "Uptime", SYSTEM_ACCENT_COLOR);
    p->rows[SYS_ROW_SYS_HEAP] = row_create(LV_SYMBOL_DRIVE, "Sys heap", SYSTEM_ACCENT_COLOR);
    p->rows[SYS_ROW_LVGL_HEAP] = row_create(LV_SYMBOL_IMAGE, "LVGL heap", SYSTEM_ACCENT_COLOR);
    p->rows[SYS_ROW_SD] = row_create(LV_SYMBOL_SD_CARD, "SD card", SYSTEM_ACCENT_COLOR);
    p->rows[SYS_ROW_ROLL] = row_create(LV_SYMBOL_GPS, "Roll", SYSTEM_ACCENT_COLOR);
    p->rows[SYS_ROW_PITCH] = row_create(LV_SYMBOL_GPS, "Pitch", SYSTEM_ACCENT_COLOR);

    // 先收集執行緒再建立列，不能在 foreach callback 裡呼叫 LVGL
    k_thread_foreach_unlocked(thread_collect_cb, NULL);
    for (int i = 0; i < p->thread_count; i++) {
        const char *name = k_thread_name_get(p->threads[i]);
        char fallback[16];

        if (name == NULL || name[0] == '\0') {
            snprintf(fallback, sizeof(fallback), "%p", (void *)p->threads[i]);
            name = fallback;
        }
        p->thread_rows[i] = row_create(LV_SYMBOL_SETTINGS, name, SYSTEM_ACCENT_COLOR);
    }

    ui_list_item(self->list, LV_SYMBOL_HOME, "Board", CONFIG_BOARD, lv_color_hex(SYSTEM_ACCENT_COLOR));
    ui_list_item(self->list, LV_SYMBOL_EYE_OPEN, "Zephyr", KERNEL_VERSION_STRING, lv_color_hex(SYSTEM_ACCENT_COLOR));
}

/* ---------- NETWORK ---------- */

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

static void net_refresh(void)
{
    net_page_t *p = &self->page.net;
    wlan_info_t info;
    bool linked;

    wlan_info(&info);
    linked = info.state == WIFI_STATE_COMPLETED;

    ui_label_set_fmt(p->rows[NET_ROW_STATUS], "%s", net_status_text(&info));
    ui_label_set_fmt(p->rows[NET_ROW_SSID], "%s", linked ? info.ssid : "-");
    if (linked) {
        ui_label_set_fmt(p->rows[NET_ROW_SIGNAL], "%d dBm", info.rssi);
        ui_label_set_fmt(p->rows[NET_ROW_CHANNEL], "%u", info.channel);
    }
    else {
        ui_label_set_fmt(p->rows[NET_ROW_SIGNAL], "-");
        ui_label_set_fmt(p->rows[NET_ROW_CHANNEL], "-");
    }
    ui_label_set_fmt(p->rows[NET_ROW_IP], "%s", info.ip[0] ? info.ip : "-");
    ui_label_set_fmt(p->rows[NET_ROW_NETMASK], "%s", info.netmask[0] ? info.netmask : "-");
    ui_label_set_fmt(p->rows[NET_ROW_GATEWAY], "%s", info.gateway[0] ? info.gateway : "-");
    ui_label_set_fmt(p->rows[NET_ROW_MAC], "%02x:%02x:%02x:%02x:%02x:%02x", info.mac[0], info.mac[1], info.mac[2],
                     info.mac[3], info.mac[4], info.mac[5]);

    // 沒連上時畫空點，曲線會斷開
    int32_t point = linked ? CLAMP(info.rssi, NET_RSSI_MIN, NET_RSSI_MAX) : LV_CHART_POINT_NONE;
    lv_chart_set_next_value(p->chart, p->series, point);
}

// 用最新的掃描結果重建清單最後面的幾列
static void net_show_scan_result(void)
{
    net_page_t *p = &self->page.net;
    wlan_ap_t aps[WLAN_SCAN_MAX];

    for (int i = 0; i < p->scan_row_count; i++) {
        lv_obj_delete(p->scan_rows[i]);
    }
    p->scan_row_count = wlan_scan_get(aps, ARRAY_SIZE(aps));
    for (int i = 0; i < p->scan_row_count; i++) {
        char detail[16];
        snprintf(detail, sizeof(detail), "%d dBm", aps[i].rssi);
        p->scan_rows[i] = ui_list_item(self->list, LV_SYMBOL_WIFI, aps[i].ssid, detail,
                                       lv_color_hex(NET_ACCENT_COLOR));
    }
    ui_label_set_fmt(p->scan_detail, "%d found", p->scan_row_count);
}

static void net_scan_cb(lv_event_t *e)
{
    self->page.net.pending_scan = true;
}

static lv_obj_t *net_chart_create(void)
{
    net_page_t *p = &self->page.net;
    lv_obj_t *obj = lv_chart_create(self->list);

    lv_obj_set_size(obj, LV_PCT(100), NET_CHART_HEIGHT);
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
    return obj;
}

static void net_create(void)
{
    net_page_t *p = &self->page.net;

    // 訊號強度曲線放在清單最上面，往下捲時一起捲走；切換分頁後重新開始畫
    p->chart = net_chart_create();

    p->rows[NET_ROW_STATUS] = row_create(LV_SYMBOL_OK, "Status", NET_ACCENT_COLOR);
    p->rows[NET_ROW_SSID] = row_create(LV_SYMBOL_WIFI, "SSID", NET_ACCENT_COLOR);
    p->rows[NET_ROW_SIGNAL] = row_create(LV_SYMBOL_BARS, "Signal", NET_ACCENT_COLOR);
    p->rows[NET_ROW_CHANNEL] = row_create(LV_SYMBOL_SHUFFLE, "Channel", NET_ACCENT_COLOR);
    p->rows[NET_ROW_IP] = row_create(LV_SYMBOL_HOME, "IP", NET_ACCENT_COLOR);
    p->rows[NET_ROW_NETMASK] = row_create(LV_SYMBOL_LIST, "Netmask", NET_ACCENT_COLOR);
    p->rows[NET_ROW_GATEWAY] = row_create(LV_SYMBOL_UPLOAD, "Gateway", NET_ACCENT_COLOR);
    p->rows[NET_ROW_MAC] = row_create(LV_SYMBOL_SETTINGS, "MAC", NET_ACCENT_COLOR);

    p->scan_detail = row_create(LV_SYMBOL_REFRESH, "Scan", NET_ACCENT_COLOR);
    lv_obj_t *item = lv_obj_get_parent(p->scan_detail);
    lv_obj_add_event_cb(item, net_scan_cb, LV_EVENT_CLICKED, NULL);
    ui_label_set_fmt(p->scan_detail, "press OK");
}

static void net_run(void)
{
    net_page_t *p = &self->page.net;

    if (p->pending_scan) {
        p->pending_scan = false;
        int ret = wlan_scan();
        if (ret == 0 || ret == -EBUSY) {
            p->scanning = true;
            ui_label_set_fmt(p->scan_detail, "Scanning...");
        }
        else {
            ui_label_set_fmt(p->scan_detail, "Failed (%d)", ret);
        }
    }
    if (p->scanning && !wlan_scan_busy()) {
        p->scanning = false;
        net_show_scan_result();
    }
}

/* ---------- 分頁 ---------- */

static void refresh(int64_t now)
{
    self->last_refresh = now;
    if (self->mode == MODE_SYSTEM) {
        sys_refresh(now);
    }
    else {
        net_refresh();
    }
}

// 刪掉目前的清單，建立 mode 那一頁
static void page_show(int mode)
{
    self->mode = mode;
    ui_tabs_select(self->tabs, mode);
    if (self->list != NULL) {
        lv_obj_delete(self->list);
    }
    memset(&self->page, 0, sizeof(self->page));

    self->list = ui_list_create(self->scr);
    lv_obj_set_height(self->list, UI_SCREEN_SIZE - UI_TABS_BOTTOM - 4);
    if (mode == MODE_SYSTEM) {
        sys_create();
    }
    else {
        net_create();
    }
    refresh(k_uptime_get());
}

// 敲一下切換分頁；next / prev / OK 照常在清單裡用
static bool nav_cb(nav_cmd_t cmd)
{
    if (cmd != NAV_TAP) {
        return false;
    }
    self->pending_switch = true;
    return true;
}

static void app_system_init(void *scr)
{
    uint8_t saved;

    ui_screen_init(scr);
    self->scr = scr;
    self->saved_mode = MODE_SYSTEM;
    if (prefs_load("system/mode", &saved, sizeof(saved)) == sizeof(saved) && saved == MODE_NETWORK) {
        self->saved_mode = MODE_NETWORK;
    }

    static const char *const names[] = {"SYSTEM", "NETWORK"};
    self->tabs = ui_tabs(scr, names, ARRAY_SIZE(names), lv_color_hex(SYSTEM_ACCENT_COLOR), NULL, NULL);
    page_show(self->saved_mode);
    nav_set_handler(nav_cb);
}

static void app_system_run(void *scr)
{
    int64_t now = k_uptime_get();

    if (self->pending_switch) {
        self->pending_switch = false;
        page_show((self->mode + 1) % MODE_COUNT);
        return;
    }
    if (self->mode == MODE_NETWORK) {
        net_run();
    }
    if (now - self->last_refresh >= (self->mode == MODE_SYSTEM ? SYS_REFRESH_MS : NET_REFRESH_MS)) {
        refresh(now);
    }
}

static void app_system_exit(void *scr)
{
    if (self->mode != self->saved_mode) {
        uint8_t saved = self->mode;
        prefs_save("system/mode", &saved, sizeof(saved));
    }
}
