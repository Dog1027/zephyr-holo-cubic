#include "fg_app.h"
#include "ui.h"
#include "wlan.h"
#include <zephyr/net/wifi.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

APP_FG_REGISTER(Network, LV_SYMBOL_WIFI, app_net_init, app_net_run, app_net_exit, 14);

#define NET_ACCENT_COLOR    0x4f8ef7
#define NET_REFRESH_MS      1000
#define NET_CHART_POINTS    60      // 每秒一點，一分鐘
#define NET_CHART_HEIGHT    56
#define NET_RSSI_MIN        (-100)
#define NET_RSSI_MAX        (-20)
#define NET_TEXT_MAX        24

enum {
    ROW_STATUS,
    ROW_SSID,
    ROW_SIGNAL,
    ROW_CHANNEL,
    ROW_IP,
    ROW_NETMASK,
    ROW_GATEWAY,
    ROW_MAC,
    ROW_COUNT,
};

typedef struct {
    lv_obj_t *detail;
    char text[NET_TEXT_MAX];
} net_row_t;

// DRAM region 0 幾乎滿了，app 的狀態放 noinit（region 1），init 時全部重設
static net_row_t rows[ROW_COUNT] __noinit;
static lv_obj_t *scan_rows[WLAN_SCAN_MAX] __noinit;
static wlan_ap_t aps[WLAN_SCAN_MAX] __noinit;
static int scan_row_count;
static lv_obj_t *list;
static lv_obj_t *chart;
static lv_chart_series_t *series;
static net_row_t scan_item;
static bool pending_scan;
static bool scanning;
static int64_t last_refresh;

// 只有文字變了才更新 label，避免每次都重繪
static void row_set(net_row_t *row, const char *fmt, ...)
{
    char buf[NET_TEXT_MAX];
    va_list args;

    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    if (row->detail == NULL || strcmp(buf, row->text) == 0) {
        return;
    }
    strcpy(row->text, buf);
    lv_label_set_text(row->detail, buf);
}

static lv_obj_t *row_create(const char *icon, const char *name, net_row_t *row)
{
    lv_obj_t *item = ui_list_item(list, icon, name, "-", lv_color_hex(NET_ACCENT_COLOR));

    row->detail = lv_obj_get_child(item, 2);
    strcpy(row->text, "-");
    return item;
}

static const char *status_text(const wlan_info_t *info)
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

static void refresh(void)
{
    wlan_info_t info;
    bool linked;

    wlan_info(&info);
    linked = info.state == WIFI_STATE_COMPLETED;

    row_set(&rows[ROW_STATUS], "%s", status_text(&info));
    row_set(&rows[ROW_SSID], "%s", linked ? info.ssid : "-");
    if (linked) {
        row_set(&rows[ROW_SIGNAL], "%d dBm", info.rssi);
        row_set(&rows[ROW_CHANNEL], "%u", info.channel);
    }
    else {
        row_set(&rows[ROW_SIGNAL], "-");
        row_set(&rows[ROW_CHANNEL], "-");
    }
    row_set(&rows[ROW_IP], "%s", info.ip[0] ? info.ip : "-");
    row_set(&rows[ROW_NETMASK], "%s", info.netmask[0] ? info.netmask : "-");
    row_set(&rows[ROW_GATEWAY], "%s", info.gateway[0] ? info.gateway : "-");
    row_set(&rows[ROW_MAC], "%02x:%02x:%02x:%02x:%02x:%02x", info.mac[0], info.mac[1], info.mac[2],
            info.mac[3], info.mac[4], info.mac[5]);

    // 沒連上時畫空點，曲線會斷開
    int32_t point = linked ? CLAMP(info.rssi, NET_RSSI_MIN, NET_RSSI_MAX) : LV_CHART_POINT_NONE;
    lv_chart_set_next_value(chart, series, point);
}

// 用最新的掃描結果重建清單最後面的幾列
static void show_scan_result(void)
{
    for (int i = 0; i < scan_row_count; i++) {
        lv_obj_delete(scan_rows[i]);
    }
    scan_row_count = wlan_scan_get(aps, ARRAY_SIZE(aps));
    for (int i = 0; i < scan_row_count; i++) {
        char detail[16];
        snprintf(detail, sizeof(detail), "%d dBm", aps[i].rssi);
        scan_rows[i] = ui_list_item(list, LV_SYMBOL_WIFI, aps[i].ssid, detail,
                                    lv_color_hex(NET_ACCENT_COLOR));
    }
    row_set(&scan_item, "%d found", scan_row_count);
}

static void scan_cb(lv_event_t *e)
{
    pending_scan = true;
}

static lv_obj_t *chart_create(void)
{
    lv_obj_t *obj = lv_chart_create(list);
    lv_obj_set_size(obj, LV_PCT(100), NET_CHART_HEIGHT);
    lv_obj_set_clickable(obj, false);
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 2, 0);
    lv_obj_set_style_line_color(obj, lv_color_hex(UI_DIM_COLOR), LV_PART_MAIN);
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
    series = lv_chart_add_series(obj, lv_color_hex(NET_ACCENT_COLOR), LV_CHART_AXIS_PRIMARY_Y);
    lv_chart_set_all_values(obj, series, LV_CHART_POINT_NONE);
    return obj;
}

static void app_net_init(void *scr)
{
    ui_screen_init(scr);
    ui_title(scr, "Network");
    list = ui_list_create(scr);

    memset(rows, 0, sizeof(rows));
    memset(scan_rows, 0, sizeof(scan_rows));
    scan_row_count = 0;
    pending_scan = false;
    scanning = false;

    // 訊號強度曲線放在清單最上面，往下捲時一起捲走
    chart = chart_create();

    row_create(LV_SYMBOL_OK, "Status", &rows[ROW_STATUS]);
    row_create(LV_SYMBOL_WIFI, "SSID", &rows[ROW_SSID]);
    row_create(LV_SYMBOL_BARS, "Signal", &rows[ROW_SIGNAL]);
    row_create(LV_SYMBOL_SHUFFLE, "Channel", &rows[ROW_CHANNEL]);
    row_create(LV_SYMBOL_HOME, "IP", &rows[ROW_IP]);
    row_create(LV_SYMBOL_LIST, "Netmask", &rows[ROW_NETMASK]);
    row_create(LV_SYMBOL_UPLOAD, "Gateway", &rows[ROW_GATEWAY]);
    row_create(LV_SYMBOL_SETTINGS, "MAC", &rows[ROW_MAC]);

    lv_obj_t *item = row_create(LV_SYMBOL_REFRESH, "Scan", &scan_item);
    lv_obj_add_event_cb(item, scan_cb, LV_EVENT_CLICKED, NULL);
    row_set(&scan_item, "press OK");

    refresh();
    last_refresh = k_uptime_get();
}

static void app_net_run(void *scr)
{
    if (pending_scan) {
        pending_scan = false;
        int ret = wlan_scan();
        if (ret == 0 || ret == -EBUSY) {
            scanning = true;
            row_set(&scan_item, "Scanning...");
        }
        else {
            row_set(&scan_item, "Failed (%d)", ret);
        }
    }
    if (scanning && !wlan_scan_busy()) {
        scanning = false;
        show_scan_result();
    }

    int64_t now = k_uptime_get();
    if (now - last_refresh >= NET_REFRESH_MS) {
        last_refresh = now;
        refresh();
    }
}

static void app_net_exit(void *scr)
{
    // LVGL 物件由框架刪除，這裡只清掉指標
    memset(rows, 0, sizeof(rows));
    memset(&scan_item, 0, sizeof(scan_item));
    scan_row_count = 0;
    list = NULL;
    chart = NULL;
    series = NULL;
}
