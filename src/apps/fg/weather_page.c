#include "weather_page.h"
#include "net_time.h"
#include <zephyr/kernel.h>
#include <string.h>

/*
 * Clock app 的天氣分頁：今天的天氣（weather.c 在背景抓）。透過稜鏡小字看不清楚，
 * 所以只放地點、圖示、溫度、天氣描述和今天的高低溫，都是白色 18 px 以上。
 */

#define SUN_COLOR               THEME_YELLOW
// 天氣圖示本身的顏色（畫雲用），不是介面的顏色，所以留在這裡
#define CLOUD_COLOR             0xdfe6ee
#define DARK_CLOUD_COLOR        0x8a95a6
#define RAIN_COLOR              THEME_BLUE
#define ICON_SIZE               76
#define MAX_AGE_MS              (30 * 60 * 1000)   // 打開時或開著時，資料超過這個時間就重抓

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
        lv_obj_set_style_text_font(bolt, &lv_font_montserrat_18, 0);
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

static void show_data(weather_page_t *v)
{
    const weather_t *w = &v->data;

    icon_draw(v->icon, w->kind);
    lv_label_set_text(v->area, w->area);
    lv_label_set_text_fmt(v->temp, "%d°", w->temp_c);
    lv_label_set_text(v->desc, w->desc);
    if (w->day_count > 0) {
        lv_label_set_text_fmt(v->range, "%d° / %d°", w->days[0].max_c, w->days[0].min_c);
    }
    else {
        lv_label_set_text(v->range, "");
    }
}

// 沒有資料時在中間顯示原因；有資料時就不顯示狀態（舊資料在背景更新）
static void show_status(weather_page_t *v, bool have, weather_state_t state)
{
    lv_obj_set_hidden(v->data_box, !have);
    if (have) {
        lv_obj_set_hidden(v->spinner, true);
        lv_label_set_text(v->status, "");
        return;
    }

    switch (state) {
    case WEATHER_OFFLINE:
        lv_obj_set_hidden(v->spinner, true);
        lv_label_set_text(v->status, LV_SYMBOL_WIFI "  Waiting for Wi-Fi");
        break;
    case WEATHER_FAILED:
        lv_obj_set_hidden(v->spinner, true);
        lv_label_set_text_fmt(v->status, LV_SYMBOL_WARNING "  Update failed (%d)", weather_error());
        break;
    default:
        // 抓一次大約 9 秒
        lv_obj_set_hidden(v->spinner, false);
        lv_label_set_text(v->status, "Getting the weather");
        break;
    }
}

void weather_page_create(weather_page_t *v, lv_obj_t *scr)
{
    v->shown_state = -1;

    // 有資料時才顯示的部分放在一起，沒資料時整個藏起來
    v->data_box = lv_obj_create(scr);
    lv_obj_remove_style_all(v->data_box);
    lv_obj_set_size(v->data_box, LV_PCT(100), LV_PCT(100));
    lv_obj_set_scrollable(v->data_box, false);
    lv_obj_set_clickable(v->data_box, false);
    lv_obj_set_hidden(v->data_box, true);

    v->area = ui_label(v->data_box, &lv_font_montserrat_18, THEME_TEXT);
    lv_obj_set_width(v->area, UI_SCREEN_SIZE - 24);
    lv_label_set_long_mode(v->area, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(v->area, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(v->area, LV_ALIGN_TOP_MID, 0, UI_TABS_BOTTOM + 16);

    v->icon = icon_create(v->data_box, ICON_SIZE);
    lv_obj_align(v->icon, LV_ALIGN_TOP_LEFT, 24, 88);
    v->temp = ui_label(v->data_box, &lv_font_montserrat_48, THEME_TEXT);
    lv_obj_align(v->temp, LV_ALIGN_TOP_LEFT, 114, 100);

    v->desc = ui_label(v->data_box, &lv_font_montserrat_18, THEME_TEXT);
    lv_obj_set_width(v->desc, UI_SCREEN_SIZE - 24);
    lv_label_set_long_mode(v->desc, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(v->desc, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(v->desc, LV_ALIGN_TOP_MID, 0, 176);

    v->range = ui_label(v->data_box, &lv_font_montserrat_18, THEME_TEXT);
    lv_obj_align(v->range, LV_ALIGN_TOP_MID, 0, 204);

    v->spinner = lv_spinner_create(scr);
    lv_obj_set_size(v->spinner, 72, 72);
    lv_obj_align(v->spinner, LV_ALIGN_CENTER, 0, UI_TABS_CENTER_Y - 16);
    lv_obj_set_style_arc_width(v->spinner, 6, LV_PART_MAIN);
    lv_obj_set_style_arc_color(v->spinner, lv_color_hex(THEME_TRACK), LV_PART_MAIN);
    lv_obj_set_style_arc_width(v->spinner, 6, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(v->spinner, lv_color_hex(WEATHER_ACCENT_COLOR), LV_PART_INDICATOR);
    lv_obj_set_hidden(v->spinner, true);

    v->status = ui_label(scr, &lv_font_montserrat_18, THEME_TEXT);
    lv_obj_set_style_text_align(v->status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(v->status, LV_ALIGN_CENTER, 0, UI_TABS_CENTER_Y + 44);
}

void weather_page_refresh(void)
{
    weather_update();
}

void weather_page_run(weather_page_t *v)
{
    // 沒有資料或太舊就請背景抓；失敗後模組自己會等一分鐘才再試
    weather_request(MAX_AGE_MS);

    bool have = weather_get(&v->data);
    weather_state_t state = weather_state();
    if (have && v->data.generation != v->shown_generation) {
        v->shown_generation = v->data.generation;
        show_data(v);
        v->shown_state = -1;
    }
    if (state != v->shown_state || have != v->shown_have) {
        v->shown_state = state;
        v->shown_have = have;
        show_status(v, have, state);
    }
}
