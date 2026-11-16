#include "fg_app.h"
#include "ui.h"
#include "nav.h"
#include "weather.h"
#include <string.h>

APP_FG_REGISTER(Weather, LV_SYMBOL_TINT, app_weather_init, app_weather_run, app_weather_exit, 5);

#define WEATHER_ACCENT_COLOR    0xffc83d
#define SUN_COLOR               0xffc83d
#define CLOUD_COLOR             0xdfe6ee
#define DARK_CLOUD_COLOR        0x8a95a6
#define RAIN_COLOR              0x4f8ef7
#define BIG_ICON_SIZE           76
#define SMALL_ICON_SIZE         30
#define DAY_COLUMN_WIDTH        76
#define STATUS_REFRESH_MS       5000

static const char *const wday_names[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};

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
} view_t;

// DRAM region 0 幾乎滿了，放 noinit，init 時重設
static view_t v __noinit;

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

static lv_obj_t *label_create(lv_obj_t *parent, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, "");
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    return label;
}

static void show_data(void)
{
    const weather_t *w = &v.data;

    icon_draw(v.icon, w->kind);
    lv_label_set_text_fmt(v.temp, "%d°", w->temp_c);
    lv_label_set_text(v.desc, w->desc);
    lv_label_set_text_fmt(v.detail, "Feels %d°   %d%%   %d km/h %s", w->feels_c, w->humidity,
                          w->wind_kmph, w->wind_dir);
    for (int i = 0; i < WEATHER_DAYS; i++) {
        day_view_t *d = &v.days[i];
        if (i >= w->day_count) {
            lv_obj_set_hidden(d->icon, true);
            lv_label_set_text(d->name, "");
            lv_label_set_text(d->temp, "");
            continue;
        }
        lv_obj_set_hidden(d->icon, false);
        icon_draw(d->icon, w->days[i].kind);
        lv_label_set_text(d->name, i == 0 ? "Today" : wday_names[w->days[i].wday]);
        lv_label_set_text_fmt(d->temp, "%d° / %d°", w->days[i].max_c, w->days[i].min_c);
    }
}

// 最上面一行：地點和多久以前更新；沒有資料時中間顯示原因
static void show_status(bool have, weather_state_t state)
{
    if (have) {
        int64_t minutes = (k_uptime_get() - v.data.updated_ms) / 60000;
        if (state == WEATHER_LOADING) {
            lv_label_set_text_fmt(v.area, "%s  " LV_SYMBOL_BULLET "  Updating...", v.data.area);
        }
        else if (minutes < 1) {
            lv_label_set_text_fmt(v.area, "%s  " LV_SYMBOL_BULLET "  just now", v.data.area);
        }
        else {
            lv_label_set_text_fmt(v.area, "%s  " LV_SYMBOL_BULLET "  %d min ago", v.data.area,
                                  (int)minutes);
        }
        lv_label_set_text(v.status, "");
        return;
    }

    lv_label_set_text(v.area, "Weather");
    switch (state) {
    case WEATHER_OFFLINE:
        lv_label_set_text(v.status, "Waiting for Wi-Fi");
        break;
    case WEATHER_FAILED:
        lv_label_set_text_fmt(v.status, "Update failed (%d)\nretrying in a minute", weather_error());
        break;
    default:
        lv_label_set_text(v.status, "Loading...");
        break;
    }
}

// OK 或敲一下馬上更新
static bool nav_cb(nav_cmd_t cmd)
{
    if (cmd == NAV_OK || cmd == NAV_TAP) {
        v.pending_update = true;
        return true;
    }
    return false;
}

static void app_weather_init(void *scr)
{
    memset(&v, 0, sizeof(v));
    v.shown_state = -1;
    ui_screen_init(scr);

    v.area = label_create(scr, &lv_font_montserrat_14, UI_DIM_COLOR);
    lv_obj_align(v.area, LV_ALIGN_TOP_MID, 0, 14);

    v.icon = icon_create(scr, BIG_ICON_SIZE);
    lv_obj_align(v.icon, LV_ALIGN_TOP_LEFT, 22, 38);
    v.temp = label_create(scr, &lv_font_montserrat_48, 0xffffff);
    lv_obj_align(v.temp, LV_ALIGN_TOP_LEFT, 112, 50);

    v.desc = label_create(scr, &lv_font_montserrat_18, 0xffffff);
    lv_obj_set_width(v.desc, UI_SCREEN_SIZE - 24);
    lv_label_set_long_mode(v.desc, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(v.desc, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(v.desc, LV_ALIGN_TOP_MID, 0, 120);
    v.detail = label_create(scr, &lv_font_montserrat_14, UI_DIM_COLOR);
    lv_obj_align(v.detail, LV_ALIGN_TOP_MID, 0, 146);

    // 三天預報，一天一欄
    for (int i = 0; i < WEATHER_DAYS; i++) {
        day_view_t *d = &v.days[i];
        int32_t x = (i - 1) * DAY_COLUMN_WIDTH;
        d->name = label_create(scr, &lv_font_montserrat_14, WEATHER_ACCENT_COLOR);
        lv_obj_align(d->name, LV_ALIGN_TOP_MID, x, 172);
        d->icon = icon_create(scr, SMALL_ICON_SIZE);
        lv_obj_align(d->icon, LV_ALIGN_TOP_MID, x, 190);
        d->temp = label_create(scr, &lv_font_montserrat_14, 0xffffff);
        lv_obj_align(d->temp, LV_ALIGN_TOP_MID, x, 220);
    }

    v.status = label_create(scr, &lv_font_montserrat_18, UI_DIM_COLOR);
    lv_obj_set_style_text_align(v.status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(v.status);

    nav_set_handler(nav_cb);
}

static void app_weather_run(void *scr)
{
    if (v.pending_update) {
        v.pending_update = false;
        weather_update();
    }

    bool have = weather_get(&v.data);
    weather_state_t state = weather_state();
    if (have && v.data.generation != v.shown_generation) {
        v.shown_generation = v.data.generation;
        show_data();
        v.shown_state = -1;
    }

    // 狀態每 5 秒或改變時才重畫（「幾分鐘前」）
    int64_t now = k_uptime_get();
    if (state != v.shown_state || now - v.last_status >= STATUS_REFRESH_MS) {
        v.shown_state = state;
        v.last_status = now;
        show_status(have, state);
    }
}

static void app_weather_exit(void *scr)
{
    // LVGL 物件由框架刪除
    memset(&v, 0, sizeof(v));
}
