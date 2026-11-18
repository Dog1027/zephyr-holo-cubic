#include "fg_app.h"
#include "ui.h"
#include "led.h"
#include <math.h>

#define LIGHT_RING_RADIUS   98
#define LIGHT_DOT_SIZE      22
#define LIGHT_PREVIEW_SIZE  110
#define LIGHT_OFF           (ARRAY_SIZE(colors) - 1)

typedef struct {
    const char *name;
    uint32_t rgb;
} light_color_t;

static const light_color_t colors[] = {
    {"Red", 0xff3b30},    {"Orange", 0xff9500}, {"Yellow", 0xffcc00}, {"Lime", 0xa8e10c},
    {"Green", 0x34c759},  {"Mint", 0x00c7be},   {"Cyan", 0x32d6ff},   {"Sky", 0x0a84ff},
    {"Blue", 0x3a3aff},   {"Purple", 0x8e44ff}, {"Magenta", 0xff2dd1}, {"Pink", 0xff6b9a},
    {"White", 0xffffff},  {"Off", 0x000000},
};

// 常駐（不放 self）：style 只在第一次打開時初始化，之後每次都沿用
static lv_style_t style_dot;
static lv_style_t style_dot_focused;
static lv_style_t style_dot_checked;
static bool styles_inited;

typedef struct {
    lv_obj_t *dots[ARRAY_SIZE(colors)];
    lv_obj_t *preview;
    lv_obj_t *name_label;
    int saved;      // 確認過的顏色，init 時從 led.c 的基礎顏色讀回來（離開 app 後 LED 繼續亮）
} self_t;

APP_FG_REGISTER(Light, ICON_LIGHT, app_light_init, app_light_run, app_light_exit, 11, self_t);

static void styles_init(void)
{
    if (styles_inited) {
        return;
    }
    styles_inited = true;

    lv_style_init(&style_dot);
    lv_style_set_size(&style_dot, LIGHT_DOT_SIZE, LIGHT_DOT_SIZE);
    lv_style_set_radius(&style_dot, LV_RADIUS_CIRCLE);
    lv_style_set_bg_opa(&style_dot, LV_OPA_COVER);
    lv_style_set_border_color(&style_dot, lv_color_hex(THEME_TEXT_DIM));

    lv_style_init(&style_dot_focused);
    lv_style_set_transform_width(&style_dot_focused, 5);
    lv_style_set_transform_height(&style_dot_focused, 5);
    lv_style_set_border_color(&style_dot_focused, lv_color_white());
    lv_style_set_border_width(&style_dot_focused, 2);

    lv_style_init(&style_dot_checked);
    lv_style_set_outline_color(&style_dot_checked, lv_color_white());
    lv_style_set_outline_width(&style_dot_checked, 2);
    lv_style_set_outline_pad(&style_dot_checked, 3);
}

static void preview_show(int index)
{
    lv_obj_set_style_bg_color(self->preview, lv_color_hex(colors[index].rgb), 0);
    lv_obj_set_style_border_width(self->preview, index == LIGHT_OFF ? 2 : 0, 0);
    lv_label_set_text_fmt(self->name_label, "%s%s", colors[index].name, index == self->saved ? "  " LV_SYMBOL_OK : "");
    // 文字顏色設在 preview 上，label 繼承
    lv_obj_set_style_text_color(self->preview, lv_color_hex(index == LIGHT_OFF ? THEME_TEXT : THEME_BG), 0);
}

static void dot_focused_cb(lv_event_t *e)
{
    int index = (int)(intptr_t)lv_event_get_user_data(e);
    preview_show(index);
    // 預覽：highlight 到哪個顏色 LED 就先亮哪個
    led_set_hex(colors[index].rgb);
}

static void dot_clicked_cb(lv_event_t *e)
{
    int index = (int)(intptr_t)lv_event_get_user_data(e);

    lv_obj_remove_state(self->dots[self->saved], LV_STATE_CHECKED);
    self->saved = index;
    lv_obj_add_state(self->dots[self->saved], LV_STATE_CHECKED);
    led_set_base(colors[self->saved].rgb);
    preview_show(index);
}

static void app_light_init(void *scr)
{
    styles_init();
    ui_screen_init(scr);

    self->saved = LIGHT_OFF;
    for (int i = 0; i < (int)ARRAY_SIZE(colors); i++) {
        if (colors[i].rgb == led_get_base()) {
            self->saved = i;
            break;
        }
    }

    self->preview = lv_obj_create(scr);
    lv_obj_remove_style_all(self->preview);
    lv_obj_set_size(self->preview, LIGHT_PREVIEW_SIZE, LIGHT_PREVIEW_SIZE);
    lv_obj_set_style_radius(self->preview, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(self->preview, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(self->preview, lv_color_hex(THEME_TEXT_DIM), 0);
    lv_obj_center(self->preview);
    lv_obj_set_scrollable(self->preview, false);
    lv_obj_set_clickable(self->preview, false);

    self->name_label = lv_label_create(self->preview);
    lv_obj_center(self->name_label);

    // 從正上方開始順時針排一圈
    for (int i = 0; i < (int)ARRAY_SIZE(colors); i++) {
        float a = 2.0f * 3.14159265f * i / ARRAY_SIZE(colors);
        lv_obj_t *dot = lv_obj_create(scr);
        lv_obj_remove_style_all(dot);
        lv_obj_add_style(dot, &style_dot, 0);
        lv_obj_add_style(dot, &style_dot_focused, LV_STATE_FOCUSED);
        lv_obj_add_style(dot, &style_dot_checked, LV_STATE_CHECKED);
        lv_obj_set_style_bg_color(dot, lv_color_hex(colors[i].rgb), 0);
        // 可捲動的物件被 encoder 按下時會進入編輯模式而不是點選
        lv_obj_set_scrollable(dot, false);
        if (i == LIGHT_OFF) {
            lv_obj_set_style_border_width(dot, 1, 0);
        }
        lv_obj_align(dot, LV_ALIGN_CENTER, (int32_t)(LIGHT_RING_RADIUS * sinf(a)),
                     (int32_t)(-LIGHT_RING_RADIUS * cosf(a)));
        lv_obj_add_event_cb(dot, dot_focused_cb, LV_EVENT_FOCUSED, (void *)(intptr_t)i);
        lv_obj_add_event_cb(dot, dot_clicked_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        self->dots[i] = dot;
    }

    lv_obj_add_state(self->dots[self->saved], LV_STATE_CHECKED);
    // 全部建好才加入 group，第一個加入的會被 focus 並觸發 preview_show
    for (int i = 0; i < (int)ARRAY_SIZE(colors); i++) {
        lv_group_add_obj(lv_group_get_default(), self->dots[i]);
    }
    lv_group_focus_obj(self->dots[self->saved]);
}

static void app_light_run(void *scr)
{
}

static void app_light_exit(void *scr)
{
    // 只是預覽沒按確認的話，回到確認過的顏色
    led_restore();
}
