#include "fg_app.h"
#include "ui.h"
#include "wlan.h"
#include <zephyr/net/wifi.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

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

// app 開著時的狀態，放在共用的 app_state_mem（fg_app.h），每次打開前清成 0
typedef struct {
    net_row_t rows[ROW_COUNT];
    lv_obj_t *scan_rows[WLAN_SCAN_MAX];
    wlan_ap_t aps[WLAN_SCAN_MAX];
    int scan_row_count;
    lv_obj_t *list;
    lv_obj_t *chart;
    lv_chart_series_t *series;
    net_row_t scan_item;
    bool pending_scan;
    bool scanning;
    int64_t last_refresh;
} self_t;

APP_FG_REGISTER(Network, LV_SYMBOL_WIFI, app_net_init, app_net_run, app_net_exit, 14, self_t);

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
    lv_obj_t *item = ui_list_item(self->list, icon, name, "-", lv_color_hex(NET_ACCENT_COLOR));

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

    row_set(&self->rows[ROW_STATUS], "%s", status_text(&info));
    row_set(&self->rows[ROW_SSID], "%s", linked ? info.ssid : "-");
    if (linked) {
        row_set(&self->rows[ROW_SIGNAL], "%d dBm", info.rssi);
        row_set(&self->rows[ROW_CHANNEL], "%u", info.channel);
    }
    else {
        row_set(&self->rows[ROW_SIGNAL], "-");
        row_set(&self->rows[ROW_CHANNEL], "-");
    }
    row_set(&self->rows[ROW_IP], "%s", info.ip[0] ? info.ip : "-");
    row_set(&self->rows[ROW_NETMASK], "%s", info.netmask[0] ? info.netmask : "-");
    row_set(&self->rows[ROW_GATEWAY], "%s", info.gateway[0] ? info.gateway : "-");
    row_set(&self->rows[ROW_MAC], "%02x:%02x:%02x:%02x:%02x:%02x", info.mac[0], info.mac[1], info.mac[2],
            info.mac[3], info.mac[4], info.mac[5]);

    // 沒連上時畫空點，曲線會斷開
    int32_t point = linked ? CLAMP(info.rssi, NET_RSSI_MIN, NET_RSSI_MAX) : LV_CHART_POINT_NONE;
    lv_chart_set_next_value(self->chart, self->series, point);
}

// 用最新的掃描結果重建清單最後面的幾列
static void show_scan_result(void)
{
    for (int i = 0; i < self->scan_row_count; i++) {
        lv_obj_delete(self->scan_rows[i]);
    }
    self->scan_row_count = wlan_scan_get(self->aps, ARRAY_SIZE(self->aps));
    for (int i = 0; i < self->scan_row_count; i++) {
        char detail[16];
        snprintf(detail, sizeof(detail), "%d dBm", self->aps[i].rssi);
        self->scan_rows[i] = ui_list_item(self->list, LV_SYMBOL_WIFI, self->aps[i].ssid, detail,
                                    lv_color_hex(NET_ACCENT_COLOR));
    }
    row_set(&self->scan_item, "%d found", self->scan_row_count);
}

static void scan_cb(lv_event_t *e)
{
    self->pending_scan = true;
}

static lv_obj_t *chart_create(void)
{
    lv_obj_t *obj = lv_chart_create(self->list);
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
    self->series = lv_chart_add_series(obj, lv_color_hex(NET_ACCENT_COLOR), LV_CHART_AXIS_PRIMARY_Y);
    lv_chart_set_all_values(obj, self->series, LV_CHART_POINT_NONE);
    return obj;
}

static void app_net_init(void *scr)
{
    ui_screen_init(scr);
    ui_title(scr, "Network");
    self->list = ui_list_create(scr);

    // 訊號強度曲線放在清單最上面，往下捲時一起捲走
    self->chart = chart_create();

    row_create(LV_SYMBOL_OK, "Status", &self->rows[ROW_STATUS]);
    row_create(LV_SYMBOL_WIFI, "SSID", &self->rows[ROW_SSID]);
    row_create(LV_SYMBOL_BARS, "Signal", &self->rows[ROW_SIGNAL]);
    row_create(LV_SYMBOL_SHUFFLE, "Channel", &self->rows[ROW_CHANNEL]);
    row_create(LV_SYMBOL_HOME, "IP", &self->rows[ROW_IP]);
    row_create(LV_SYMBOL_LIST, "Netmask", &self->rows[ROW_NETMASK]);
    row_create(LV_SYMBOL_UPLOAD, "Gateway", &self->rows[ROW_GATEWAY]);
    row_create(LV_SYMBOL_SETTINGS, "MAC", &self->rows[ROW_MAC]);

    lv_obj_t *item = row_create(LV_SYMBOL_REFRESH, "Scan", &self->scan_item);
    lv_obj_add_event_cb(item, scan_cb, LV_EVENT_CLICKED, NULL);
    row_set(&self->scan_item, "press OK");

    refresh();
    self->last_refresh = k_uptime_get();
}

static void app_net_run(void *scr)
{
    if (self->pending_scan) {
        self->pending_scan = false;
        int ret = wlan_scan();
        if (ret == 0 || ret == -EBUSY) {
            self->scanning = true;
            row_set(&self->scan_item, "Scanning...");
        }
        else {
            row_set(&self->scan_item, "Failed (%d)", ret);
        }
    }
    if (self->scanning && !wlan_scan_busy()) {
        self->scanning = false;
        show_scan_result();
    }

    int64_t now = k_uptime_get();
    if (now - self->last_refresh >= NET_REFRESH_MS) {
        self->last_refresh = now;
        refresh();
    }
}

static void app_net_exit(void *scr)
{
    // LVGL 物件由框架刪除
}
