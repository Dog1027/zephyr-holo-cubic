#include "fg_app.h"
#include "ui.h"
#include "nav.h"
#include "prefs.h"
#include "net_time.h"
#include "wlan.h"

APP_FG_REGISTER(Clock, LV_SYMBOL_LOOP, app_clock_init, app_clock_run, app_clock_exit, 5);

#define CLOCK_ACCENT_COLOR  0x2ec4d6
#define CLOCK_ARC_SIZE      220
#define CLOCK_ARC_WIDTH     6

static const char *const weekdays[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
static const char *const months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                     "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

static lv_obj_t *arc;
static lv_obj_t *time_label;
static lv_obj_t *ampm_label;
static lv_obj_t *date_label;
static lv_obj_t *status_label;
static int64_t shown_key;   // 上次畫面對應的時間（秒），-1 表示要重畫
static bool shown_synced;
static bool hour24 = true;
static bool pending_toggle;

static lv_obj_t *label_create(lv_obj_t *scr, const lv_font_t *font, lv_color_t color)
{
    lv_obj_t *label = lv_label_create(scr);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, color, 0);
    return label;
}

// 還沒對時：說明原因
static void show_status(void)
{
    const char *text = wlan_is_online() ? "Syncing time..." : "Waiting for Wi-Fi";

    lv_label_set_text(time_label, "--:--");
    lv_label_set_text(ampm_label, "");
    lv_label_set_text(date_label, "");
    lv_label_set_text(status_label, text);
    lv_arc_set_value(arc, 0);
}

static void show_time(const struct tm *tm)
{
    int hour = tm->tm_hour;

    if (!hour24) {
        lv_label_set_text(ampm_label, hour < 12 ? "AM" : "PM");
        hour = hour % 12 == 0 ? 12 : hour % 12;
    }
    else {
        lv_label_set_text(ampm_label, "");
    }
    lv_label_set_text_fmt(time_label, "%02d:%02d", hour, tm->tm_min);
    lv_label_set_text_fmt(date_label, "%s  %d %s %d", weekdays[tm->tm_wday], tm->tm_mday,
                          months[tm->tm_mon], tm->tm_year + 1900);
    lv_label_set_text(status_label, "");
    lv_arc_set_value(arc, tm->tm_sec);
    // AM / PM 跟在時間右下方，時間的寬度會變
    lv_obj_align_to(ampm_label, time_label, LV_ALIGN_OUT_RIGHT_BOTTOM, 4, -8);
}

// OK 或敲一下切換 12 / 24 小時制
static bool nav_cb(nav_cmd_t cmd)
{
    if (cmd == NAV_OK || cmd == NAV_TAP) {
        pending_toggle = true;
        return true;
    }
    return false;
}

static void app_clock_init(void *scr)
{
    uint8_t saved;

    ui_screen_init(scr);
    if (prefs_load("clock/24h", &saved, sizeof(saved)) == sizeof(saved)) {
        hour24 = saved != 0;
    }
    // tz 可能在電腦上改過，每次打開都重讀
    net_time_load_tz();

    // 秒數圓環：0 到 59 秒走一圈
    arc = lv_arc_create(scr);
    lv_obj_set_size(arc, CLOCK_ARC_SIZE, CLOCK_ARC_SIZE);
    lv_obj_center(arc);
    lv_arc_set_rotation(arc, 270);
    lv_arc_set_bg_angles(arc, 0, 360);
    lv_arc_set_range(arc, 0, 59);
    lv_obj_remove_style(arc, NULL, LV_PART_KNOB);
    lv_obj_set_clickable(arc, false);
    lv_obj_set_style_arc_width(arc, CLOCK_ARC_WIDTH, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, CLOCK_ARC_WIDTH, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, lv_color_hex(UI_DIM_COLOR), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(arc, LV_OPA_30, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, lv_color_hex(CLOCK_ACCENT_COLOR), LV_PART_INDICATOR);

    time_label = label_create(scr, &lv_font_montserrat_48, lv_color_white());
    lv_obj_align(time_label, LV_ALIGN_CENTER, 0, -10);
    ampm_label = label_create(scr, &lv_font_montserrat_14, lv_color_hex(CLOCK_ACCENT_COLOR));
    date_label = label_create(scr, &lv_font_montserrat_18, lv_color_hex(UI_DIM_COLOR));
    lv_obj_align(date_label, LV_ALIGN_CENTER, 0, 34);
    status_label = label_create(scr, &lv_font_montserrat_14, lv_color_hex(UI_DIM_COLOR));
    lv_obj_align(status_label, LV_ALIGN_CENTER, 0, 34);

    shown_key = -1;
    pending_toggle = false;
    nav_set_handler(nav_cb);
}

static void app_clock_run(void *scr)
{
    struct tm tm;

    if (pending_toggle) {
        pending_toggle = false;
        hour24 = !hour24;
        uint8_t saved = hour24;
        prefs_save("clock/24h", &saved, sizeof(saved));
        shown_key = -1;
    }

    // 每秒最多重畫一次：對時前用開機秒數（只是更新 Wi-Fi 狀態），對時後用實際時間
    bool synced = net_time_now(&tm);
    int64_t key = synced ? net_time_unix() : k_uptime_get() / 1000;
    if (synced == shown_synced && key == shown_key) {
        return;
    }
    shown_synced = synced;
    shown_key = key;
    if (synced) {
        show_time(&tm);
    }
    else {
        show_status();
    }
}

static void app_clock_exit(void *scr)
{
}
