#include "fg_app.h"
#include "ui.h"
#include "nav.h"
#include "weather.h"
#include "net_time.h"
#include <string.h>

#define WEATHER_ACCENT_COLOR    0xffc83d
#define SUN_COLOR               0xffc83d
#define CLOUD_COLOR             0xdfe6ee
#define DARK_CLOUD_COLOR        0x8a95a6
#define RAIN_COLOR              0x4f8ef7
#define BIG_ICON_SIZE           76
#define SMALL_ICON_SIZE         30
#define DAY_COLUMN_WIDTH        76
#define STATUS_REFRESH_MS       5000
#define MAX_AGE_MS              (30 * 60 * 1000)   // 打開時或開著時，資料超過這個時間就重抓

typedef struct {
    lv_obj_t *icon;
    lv_obj_t *name;
    lv_obj_t *temp;
} day_view_t;

typedef struct {
    lv_obj_t *area;
    lv_obj_t *icon;
    lv_obj_t *temp;
    lv_obj_t *desc;
    lv_obj_t *detail;
    lv_obj_t *status;
    day_view_t days[WEATHER_DAYS];
    weather_t data;
    uint32_t shown_generation;
    int shown_state;
    int64_t last_status;
    bool pending_update;
} self_t;

APP_FG_REGISTER(Weather, LV_SYMBOL_TINT, app_weather_init, app_weather_run, app_weather_exit, 5, self_t);

/* ---------- 圖示：用圓形和圓角矩形拼出來，座標都是圖示大小的比例 ---------- */

static lv_obj_t *shape(lv_obj_t *parent, int32_t size, int x, int y, int w, int h, int radius,
                       uint32_t color)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_clickable(obj, false);
    lv_obj_set_scrollable(obj, false);
    // x / y / w / h 是百分比
    lv_obj_set_pos(obj, size * x / 100, size * y / 100);
    lv_obj_set_size(obj, MAX(size * w / 100, 2), MAX(size * h / 100, 2));
    lv_obj_set_style_radius(obj, radius < 0 ? LV_RADIUS_CIRCLE : size * radius / 100, 0);
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    return obj;
}

// 雲在圖示的 y 16% 到 76% 之間，下面留給雨滴或雪
static void cloud(lv_obj_t *parent, int32_t size, int dx, int dy, uint32_t color)
{
    shape(parent, size, 12 + dx, 32 + dy, 42, 42, -1, color);
    shape(parent, size, 32 + dx, 16 + dy, 54, 54, -1, color);
    shape(parent, size, 8 + dx, 48 + dy, 84, 28, 14, color);
}

static void icon_draw(lv_obj_t *icon, weather_kind_t kind)
{
    int32_t size = lv_obj_get_width(icon);

    lv_obj_clean(icon);
    switch (kind) {
    case WEATHER_SUNNY:
        shape(icon, size, 20, 20, 60, 60, -1, SUN_COLOR);
        break;
    case WEATHER_PARTLY:
        shape(icon, size, 40, 4, 50, 50, -1, SUN_COLOR);
        cloud(icon, size, -6, 12, CLOUD_COLOR);
        break;
    case WEATHER_CLOUDY:
        cloud(icon, size, 0, 6, CLOUD_COLOR);
        break;
    case WEATHER_FOG:
        for (int i = 0; i < 3; i++) {
            shape(icon, size, 10 + i * 6, 24 + i * 20, 80 - i * 12, 10, -1, CLOUD_COLOR);
        }
        break;
    case WEATHER_RAIN:
        cloud(icon, size, 0, 0, DARK_CLOUD_COLOR);
        for (int i = 0; i < 3; i++) {
            shape(icon, size, 28 + i * 18, 80, 7, 18, -1, RAIN_COLOR);
        }
        break;
    case WEATHER_SNOW:
        cloud(icon, size, 0, 0, CLOUD_COLOR);
        for (int i = 0; i < 3; i++) {
            shape(icon, size, 26 + i * 18, 82, 12, 12, -1, lv_color_to_u32(lv_color_white()));
        }
        break;
    case WEATHER_THUNDER: {
        cloud(icon, size, 0, 0, DARK_CLOUD_COLOR);
        lv_obj_t *bolt = lv_label_create(icon);
        lv_label_set_text(bolt, LV_SYMBOL_CHARGE);
        lv_obj_set_style_text_color(bolt, lv_color_hex(SUN_COLOR), 0);
        lv_obj_set_style_text_font(bolt, size > 40 ? &lv_font_montserrat_18 : &lv_font_montserrat_14,
                                   0);
        lv_obj_align(bolt, LV_ALIGN_BOTTOM_MID, 0, size / 8);
        break;
    }
    }
}

static lv_obj_t *icon_create(lv_obj_t *parent, int32_t size)
{
    lv_obj_t *icon = lv_obj_create(parent);
    lv_obj_remove_style_all(icon);
    lv_obj_set_clickable(icon, false);
    lv_obj_set_scrollable(icon, false);
    lv_obj_set_size(icon, size, size);
    return icon;
}

/* ---------- 畫面 ---------- */

static void show_data(void)
{
    const weather_t *w = &self->data;

    icon_draw(self->icon, w->kind);
    lv_label_set_text_fmt(self->temp, "%d°", w->temp_c);
    lv_label_set_text(self->desc, w->desc);
    lv_label_set_text_fmt(self->detail, "Feels %d°   %d%%   %d km/h %s", w->feels_c, w->humidity,
                          w->wind_kmph, w->wind_dir);
    for (int i = 0; i < WEATHER_DAYS; i++) {
        day_view_t *d = &self->days[i];
        if (i >= w->day_count) {
            lv_obj_set_hidden(d->icon, true);
            lv_label_set_text(d->name, "");
            lv_label_set_text(d->temp, "");
            continue;
        }
        lv_obj_set_hidden(d->icon, false);
        icon_draw(d->icon, w->days[i].kind);
        lv_label_set_text(d->name, i == 0 ? "Today" : net_time_wday_names[w->days[i].wday]);
        lv_label_set_text_fmt(d->temp, "%d° / %d°", w->days[i].max_c, w->days[i].min_c);
    }
}

// 最上面一行：地點和多久以前更新；沒有資料時中間顯示原因
static void show_status(bool have, weather_state_t state)
{
    if (have) {
        int64_t minutes = (k_uptime_get() - self->data.updated_ms) / 60000;
        if (state == WEATHER_LOADING) {
            lv_label_set_text_fmt(self->area, "%s  " LV_SYMBOL_BULLET "  Updating...", self->data.area);
        }
        else if (minutes < 1) {
            lv_label_set_text_fmt(self->area, "%s  " LV_SYMBOL_BULLET "  just now", self->data.area);
        }
        else {
            lv_label_set_text_fmt(self->area, "%s  " LV_SYMBOL_BULLET "  %d min ago", self->data.area,
                                  (int)minutes);
        }
        lv_label_set_text(self->status, "");
        return;
    }

    lv_label_set_text(self->area, "Weather");
    switch (state) {
    case WEATHER_OFFLINE:
        lv_label_set_text(self->status, "Waiting for Wi-Fi");
        break;
    case WEATHER_FAILED:
        lv_label_set_text_fmt(self->status, "Update failed (%d)\nretrying in a minute", weather_error());
        break;
    default:
        lv_label_set_text(self->status, "Loading...");
        break;
    }
}

// OK 或敲一下馬上更新
static bool nav_cb(nav_cmd_t cmd)
{
    if (cmd == NAV_OK || cmd == NAV_TAP) {
        self->pending_update = true;
        return true;
    }
    return false;
}

static void app_weather_init(void *scr)
{
    self->shown_state = -1;
    ui_screen_init(scr);

    self->area = ui_label(scr, &lv_font_montserrat_14, UI_DIM_COLOR);
    lv_obj_align(self->area, LV_ALIGN_TOP_MID, 0, 14);

    self->icon = icon_create(scr, BIG_ICON_SIZE);
    lv_obj_align(self->icon, LV_ALIGN_TOP_LEFT, 22, 38);
    self->temp = ui_label(scr, &lv_font_montserrat_48, 0xffffff);
    lv_obj_align(self->temp, LV_ALIGN_TOP_LEFT, 112, 50);

    self->desc = ui_label(scr, &lv_font_montserrat_18, 0xffffff);
    lv_obj_set_width(self->desc, UI_SCREEN_SIZE - 24);
    lv_label_set_long_mode(self->desc, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(self->desc, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(self->desc, LV_ALIGN_TOP_MID, 0, 120);
    self->detail = ui_label(scr, &lv_font_montserrat_14, UI_DIM_COLOR);
    lv_obj_align(self->detail, LV_ALIGN_TOP_MID, 0, 146);

    // 三天預報，一天一欄
    for (int i = 0; i < WEATHER_DAYS; i++) {
        day_view_t *d = &self->days[i];
        int32_t x = (i - 1) * DAY_COLUMN_WIDTH;
        d->name = ui_label(scr, &lv_font_montserrat_14, WEATHER_ACCENT_COLOR);
        lv_obj_align(d->name, LV_ALIGN_TOP_MID, x, 172);
        d->icon = icon_create(scr, SMALL_ICON_SIZE);
        lv_obj_align(d->icon, LV_ALIGN_TOP_MID, x, 190);
        d->temp = ui_label(scr, &lv_font_montserrat_14, 0xffffff);
        lv_obj_align(d->temp, LV_ALIGN_TOP_MID, x, 220);
    }

    self->status = ui_label(scr, &lv_font_montserrat_18, UI_DIM_COLOR);
    lv_obj_set_style_text_align(self->status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(self->status);

    nav_set_handler(nav_cb);
}

static void app_weather_run(void *scr)
{
    if (self->pending_update) {
        self->pending_update = false;
        weather_update();
    }

    // 沒有資料或太舊就請背景抓；失敗後模組自己會等一分鐘才再試
    weather_request(MAX_AGE_MS);

    bool have = weather_get(&self->data);
    weather_state_t state = weather_state();
    if (have && self->data.generation != self->shown_generation) {
        self->shown_generation = self->data.generation;
        show_data();
        self->shown_state = -1;
    }

    // 狀態每 5 秒或改變時才重畫（「幾分鐘前」）
    int64_t now = k_uptime_get();
    if (state != self->shown_state || now - self->last_status >= STATUS_REFRESH_MS) {
        self->shown_state = state;
        self->last_status = now;
        show_status(have, state);
    }
}

static void app_weather_exit(void *scr)
{
}
