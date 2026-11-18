#include "fg_app.h"
#include "ui.h"
#include "ui_tabs.h"
#include "prefs.h"
#include "net_time.h"
#include "wlan.h"
#include "weather_page.h"

/*
 * 時鐘和今天的天氣，兩個分頁（ui_tabs.h）。OK 或敲一下：時鐘切換 12 / 24 小時制，天氣馬上重抓。
 * 12 小時制沒有 AM / PM 字樣（小字透過稜鏡看不清楚），下午時時間換成 CLOCK_ACCENT_COLOR。
 */

#define CLOCK_ACCENT_COLOR  THEME_CYAN
#define CLOCK_ARC_SIZE      196
#define CLOCK_ARC_WIDTH     6
#define CLOCK_ARC_STEP_MS   50  // 圓環每這麼久前進一點（一步約 0.6 px），文字還是一秒更新一次

enum { MODE_CLOCK, MODE_WEATHER, MODE_COUNT };

typedef struct {
    lv_obj_t *pages[MODE_COUNT];    // 每個分頁一個容器，只顯示目前那個
    int mode;
    weather_page_t weather;
    lv_obj_t *arc;
    lv_obj_t *time_label;
    lv_obj_t *date_label;
    lv_obj_t *status_label;
    int64_t shown_key;  // 上次畫面對應的時間（秒），-1 表示要重畫
    int32_t shown_arc;
    bool shown_synced;
    bool hour24;
} self_t;

APP_FG_REGISTER(Clock, ICON_CLOCK, app_clock_init, app_clock_run, app_clock_exit, 4, self_t);

// 還沒對時：說明原因
static void show_status(void)
{
    const char *text = wlan_is_online() ? "Syncing time..." : "Waiting for Wi-Fi";

    lv_label_set_text(self->time_label, "--:--");
    lv_obj_set_style_text_color(self->time_label, lv_color_hex(THEME_TEXT), 0);
    lv_label_set_text(self->date_label, "");
    lv_label_set_text(self->status_label, text);
    lv_arc_set_value(self->arc, 0);
}

static void show_time(const struct tm *tm)
{
    int hour = tm->tm_hour;
    bool pm = !self->hour24 && hour >= 12;

    if (!self->hour24) {
        hour = hour % 12 == 0 ? 12 : hour % 12;
    }
    lv_label_set_text_fmt(self->time_label, "%02d:%02d", hour, tm->tm_min);
    lv_obj_set_style_text_color(self->time_label, lv_color_hex(pm ? CLOCK_ACCENT_COLOR : THEME_TEXT), 0);
    lv_label_set_text_fmt(self->date_label, "%s  %d %s %d", net_time_wday_names[tm->tm_wday], tm->tm_mday,
                          net_time_month_names[tm->tm_mon], tm->tm_year + 1900);
    lv_label_set_text(self->status_label, "");
}

static void tab_show(int mode)
{
    self->mode = mode;
    for (int i = 0; i < MODE_COUNT; i++) {
        lv_obj_set_hidden(self->pages[i], i != mode);
    }
}

static void tab_action(int mode, bool tap)
{
    if (mode == MODE_WEATHER) {
        weather_page_refresh();
        return;
    }
    self->hour24 = !self->hour24;
    uint8_t saved = self->hour24;
    prefs_save("clock/24h", &saved, sizeof(saved));
    self->shown_key = -1;
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

static void clock_page_create(lv_obj_t *scr)
{
    // 秒數圓環：一分鐘走一圈，單位是 CLOCK_ARC_STEP_MS
    self->arc = lv_arc_create(scr);
    lv_obj_set_size(self->arc, CLOCK_ARC_SIZE, CLOCK_ARC_SIZE);
    lv_obj_align(self->arc, LV_ALIGN_CENTER, 0, UI_TABS_CENTER_Y);
    lv_arc_set_rotation(self->arc, 270);
    lv_arc_set_bg_angles(self->arc, 0, 360);
    lv_arc_set_range(self->arc, 0, 60000 / CLOCK_ARC_STEP_MS);
    lv_obj_remove_style(self->arc, NULL, LV_PART_KNOB);
    lv_obj_set_clickable(self->arc, false);
    lv_obj_set_style_arc_width(self->arc, CLOCK_ARC_WIDTH, LV_PART_MAIN);
    lv_obj_set_style_arc_width(self->arc, CLOCK_ARC_WIDTH, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(self->arc, lv_color_hex(THEME_TEXT_DIM), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(self->arc, LV_OPA_30, LV_PART_MAIN);
    lv_obj_set_style_arc_color(self->arc, lv_color_hex(CLOCK_ACCENT_COLOR), LV_PART_INDICATOR);

    self->time_label = ui_label(scr, &lv_font_montserrat_48, THEME_TEXT);
    lv_obj_align(self->time_label, LV_ALIGN_CENTER, 0, UI_TABS_CENTER_Y - 10);
    self->date_label = ui_label(scr, &lv_font_montserrat_18, THEME_TEXT);
    lv_obj_align(self->date_label, LV_ALIGN_CENTER, 0, UI_TABS_CENTER_Y + 34);
    self->status_label = ui_label(scr, &lv_font_montserrat_18, THEME_TEXT);
    lv_obj_align(self->status_label, LV_ALIGN_CENTER, 0, UI_TABS_CENTER_Y + 34);
}

static void app_clock_init(void *scr)
{
    uint8_t saved;

    ui_screen_init(scr);
    self->hour24 = true;
    if (prefs_load("clock/24h", &saved, sizeof(saved)) == sizeof(saved)) {
        self->hour24 = saved != 0;
    }
    self->shown_key = -1;
    self->shown_arc = -1;

    self->pages[MODE_CLOCK] = page_create(scr);
    clock_page_create(self->pages[MODE_CLOCK]);
    self->pages[MODE_WEATHER] = page_create(scr);
    weather_page_create(&self->weather, self->pages[MODE_WEATHER]);

    // 建立時就會呼叫 tab_show，內容要先建好
    static const char *const names[] = {"CLOCK", "WEATHER"};
    static const ui_tabs_cfg_t tabs = {
        .names = names, .count = ARRAY_SIZE(names), .accent = CLOCK_ACCENT_COLOR,
        .pref_key = "clock/mode", .show = tab_show, .action = tab_action,
    };
    ui_tabs_create(scr, &tabs);
}

static void clock_page_run(void)
{
    struct tm tm;

    // 文字每秒最多重畫一次：對時前用開機秒數（只是更新 Wi-Fi 狀態），對時後用實際時間
    bool synced = net_time_now(&tm);
    int64_t ms = net_time_unix_ms();
    int64_t key = synced ? ms / 1000 : k_uptime_get() / 1000;

    // 圓環只重畫前進的那一小段，可以更新得比文字細
    if (synced) {
        int32_t arc = (int32_t)(ms % 60000) / CLOCK_ARC_STEP_MS;
        if (arc != self->shown_arc) {
            self->shown_arc = arc;
            lv_arc_set_value(self->arc, arc);
        }
    }
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

static void app_clock_run(void *scr)
{
    if (self->mode == MODE_WEATHER) {
        weather_page_run(&self->weather);
    }
    else {
        clock_page_run();
    }
}

static void app_clock_exit(void *scr)
{
}
