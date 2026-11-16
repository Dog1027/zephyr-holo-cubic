#include "fg_app.h"
#include "ui.h"
#include "nav.h"
#include "prefs.h"
#include "net_time.h"
#include "wlan.h"

#define CLOCK_ACCENT_COLOR  0x2ec4d6
#define CLOCK_ARC_SIZE      220
#define CLOCK_ARC_WIDTH     6

typedef struct {
    lv_obj_t *arc;
    lv_obj_t *time_label;
    lv_obj_t *ampm_label;
    lv_obj_t *date_label;
    lv_obj_t *status_label;
    int64_t shown_key;  // 上次畫面對應的時間（秒），-1 表示要重畫
    bool shown_synced;
    bool hour24;
    bool pending_toggle;
} self_t;

APP_FG_REGISTER(Clock, LV_SYMBOL_LOOP, app_clock_init, app_clock_run, app_clock_exit, 4, self_t);

// 還沒對時：說明原因
static void show_status(void)
{
    const char *text = wlan_is_online() ? "Syncing time..." : "Waiting for Wi-Fi";

    lv_label_set_text(self->time_label, "--:--");
    lv_label_set_text(self->ampm_label, "");
    lv_label_set_text(self->date_label, "");
    lv_label_set_text(self->status_label, text);
    lv_arc_set_value(self->arc, 0);
}

static void show_time(const struct tm *tm)
{
    int hour = tm->tm_hour;

    if (!self->hour24) {
        lv_label_set_text(self->ampm_label, hour < 12 ? "AM" : "PM");
        hour = hour % 12 == 0 ? 12 : hour % 12;
    }
    else {
        lv_label_set_text(self->ampm_label, "");
    }
    lv_label_set_text_fmt(self->time_label, "%02d:%02d", hour, tm->tm_min);
    lv_label_set_text_fmt(self->date_label, "%s  %d %s %d", net_time_wday_names[tm->tm_wday], tm->tm_mday,
                          net_time_month_names[tm->tm_mon], tm->tm_year + 1900);
    lv_label_set_text(self->status_label, "");
    lv_arc_set_value(self->arc, tm->tm_sec);
    // AM / PM 跟在時間右下方，時間的寬度會變
    lv_obj_align_to(self->ampm_label, self->time_label, LV_ALIGN_OUT_RIGHT_BOTTOM, 4, -8);
}

// OK 或敲一下切換 12 / 24 小時制
static bool nav_cb(nav_cmd_t cmd)
{
    if (cmd == NAV_OK || cmd == NAV_TAP) {
        self->pending_toggle = true;
        return true;
    }
    return false;
}

static void app_clock_init(void *scr)
{
    uint8_t saved;

    ui_screen_init(scr);
    self->hour24 = true;
    if (prefs_load("clock/24h", &saved, sizeof(saved)) == sizeof(saved)) {
        self->hour24 = saved != 0;
    }
    // tz 可能在電腦上改過，每次打開都重讀
    net_time_load_tz();

    // 秒數圓環：0 到 59 秒走一圈
    self->arc = lv_arc_create(scr);
    lv_obj_set_size(self->arc, CLOCK_ARC_SIZE, CLOCK_ARC_SIZE);
    lv_obj_center(self->arc);
    lv_arc_set_rotation(self->arc, 270);
    lv_arc_set_bg_angles(self->arc, 0, 360);
    lv_arc_set_range(self->arc, 0, 59);
    lv_obj_remove_style(self->arc, NULL, LV_PART_KNOB);
    lv_obj_set_clickable(self->arc, false);
    lv_obj_set_style_arc_width(self->arc, CLOCK_ARC_WIDTH, LV_PART_MAIN);
    lv_obj_set_style_arc_width(self->arc, CLOCK_ARC_WIDTH, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(self->arc, lv_color_hex(UI_DIM_COLOR), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(self->arc, LV_OPA_30, LV_PART_MAIN);
    lv_obj_set_style_arc_color(self->arc, lv_color_hex(CLOCK_ACCENT_COLOR), LV_PART_INDICATOR);

    self->time_label = ui_label(scr, &lv_font_montserrat_48, 0xffffff);
    lv_obj_align(self->time_label, LV_ALIGN_CENTER, 0, -10);
    self->ampm_label = ui_label(scr, &lv_font_montserrat_14, CLOCK_ACCENT_COLOR);
    self->date_label = ui_label(scr, &lv_font_montserrat_18, UI_DIM_COLOR);
    lv_obj_align(self->date_label, LV_ALIGN_CENTER, 0, 34);
    self->status_label = ui_label(scr, &lv_font_montserrat_14, UI_DIM_COLOR);
    lv_obj_align(self->status_label, LV_ALIGN_CENTER, 0, 34);

    self->shown_key = -1;
    nav_set_handler(nav_cb);
}

static void app_clock_run(void *scr)
{
    struct tm tm;

    if (self->pending_toggle) {
        self->pending_toggle = false;
        self->hour24 = !self->hour24;
        uint8_t saved = self->hour24;
        prefs_save("clock/24h", &saved, sizeof(saved));
        self->shown_key = -1;
    }

    // 每秒最多重畫一次：對時前用開機秒數（只是更新 Wi-Fi 狀態），對時後用實際時間
    bool synced = net_time_now(&tm);
    int64_t key = synced ? net_time_unix() : k_uptime_get() / 1000;
    if (synced == self->shown_synced && key == self->shown_key) {
        return;
    }
    self->shown_synced = synced;
    self->shown_key = key;
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
