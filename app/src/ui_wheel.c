#include <stdio.h>
#include <string.h>
#include "ui.h"
#include "ui_wheel.h"

#define WHEEL_NEIGHBOUR_OPA     LV_OPA_50   // 前後項目只是讓人知道上下是什麼，不必看清楚
#define WHEEL_BOX_Y             42          // 中間那一項從 top 往下這麼多開始
#define WHEEL_BOX_HEIGHT        110
#define WHEEL_NEXT_Y            150
#define WHEEL_VALUE_Y           26
#define WHEEL_BAR_Y             74
#define WHEEL_BAR_WIDTH         160
#define WHEEL_BAR_HEIGHT        8
#define WHEEL_SLIDE_PX          20
#define WHEEL_SLIDE_MS          150

// 一樣的文字不重設，否則每次刷新都會重畫
static void label_set(lv_obj_t *label, const char *text)
{
    if (strcmp(lv_label_get_text(label), text) != 0) {
        lv_label_set_text(label, text);
    }
}

static lv_obj_t *centered_label(lv_obj_t *parent, const lv_font_t *font, int32_t y)
{
    lv_obj_t *label = ui_label(parent, font, THEME_TEXT);
    lv_obj_set_width(label, UI_SCREEN_SIZE - 24);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(label, LV_ALIGN_TOP_MID, 0, y);
    return label;
}

static int wrap(const ui_wheel_t *w, int index)
{
    return (index % w->count + w->count) % w->count;
}

static void show(ui_wheel_t *w, bool names)
{
    ui_wheel_item_t item;

    if (w->count == 0) {
        label_set(w->prev_name, "");
        label_set(w->name, "");
        label_set(w->value, "");
        label_set(w->next_name, "");
        label_set(w->position, "");
        lv_obj_set_hidden(w->bar, true);
        return;
    }

    // 前後兩項的名稱只在換項目時更新
    if (names) {
        bool alone = w->count == 1;
        item.name[0] = '\0';
        if (!alone) {
            w->get(wrap(w, w->index - 1), &item, true);
        }
        label_set(w->prev_name, item.name);
        item.name[0] = '\0';
        if (!alone) {
            w->get(wrap(w, w->index + 1), &item, true);
        }
        label_set(w->next_name, item.name);

        char pos[16];
        snprintf(pos, sizeof(pos), "%d / %d", w->index + 1, w->count);
        label_set(w->position, pos);
    }

    memset(&item, 0, sizeof(item));
    item.percent = -1;
    w->get(w->index, &item, false);
    label_set(w->name, item.name);
    label_set(w->value, item.value);

    if (item.extra != w->shown_extra) {
        if (w->shown_extra != NULL) {
            lv_obj_set_hidden(w->shown_extra, true);
        }
        if (item.extra != NULL) {
            lv_obj_set_hidden(item.extra, false);
        }
        w->shown_extra = item.extra;
    }
    bool bar = item.extra == NULL && item.percent >= 0;
    lv_obj_set_hidden(w->bar, !bar);
    if (bar && lv_bar_get_value(w->bar) != item.percent) {
        lv_bar_set_value(w->bar, item.percent, LV_ANIM_OFF);
    }
}

static void slide_cb(void *obj, int32_t v)
{
    lv_obj_set_style_translate_y(obj, v, 0);
}

void ui_wheel_create(ui_wheel_t *w, lv_obj_t *parent, int32_t top, uint32_t accent, ui_wheel_get_t get, int count)
{
    w->get = get;
    w->count = count;
    w->index = 0;
    w->shown_extra = NULL;

    w->prev_name = centered_label(parent, &lv_font_montserrat_18, top + 10);
    lv_obj_set_style_text_opa(w->prev_name, WHEEL_NEIGHBOUR_OPA, 0);
    lv_label_set_long_mode(w->prev_name, LV_LABEL_LONG_MODE_DOTS);

    w->box = lv_obj_create(parent);
    lv_obj_remove_style_all(w->box);
    lv_obj_set_size(w->box, LV_PCT(100), WHEEL_BOX_HEIGHT);
    lv_obj_align(w->box, LV_ALIGN_TOP_MID, 0, top + WHEEL_BOX_Y);
    lv_obj_set_scrollable(w->box, false);
    lv_obj_set_clickable(w->box, false);

    w->name = centered_label(w->box, &lv_font_montserrat_18, 0);
    lv_label_set_long_mode(w->name, LV_LABEL_LONG_MODE_DOTS);
    // 太長的值（例如板子名稱）換行
    w->value = centered_label(w->box, &lv_font_montserrat_28, WHEEL_VALUE_Y);
    lv_label_set_long_mode(w->value, LV_LABEL_LONG_MODE_WRAP);

    w->bar = lv_bar_create(w->box);
    lv_obj_set_size(w->bar, WHEEL_BAR_WIDTH, WHEEL_BAR_HEIGHT);
    lv_obj_align(w->bar, LV_ALIGN_TOP_MID, 0, WHEEL_BAR_Y);
    lv_bar_set_range(w->bar, 0, 100);
    lv_obj_set_style_bg_color(w->bar, lv_color_hex(THEME_TRACK), LV_PART_MAIN);
    lv_obj_set_style_bg_color(w->bar, lv_color_hex(accent), LV_PART_INDICATOR);
    lv_obj_set_hidden(w->bar, true);

    w->next_name = centered_label(parent, &lv_font_montserrat_18, top + WHEEL_NEXT_Y);
    lv_obj_set_style_text_opa(w->next_name, WHEEL_NEIGHBOUR_OPA, 0);
    lv_label_set_long_mode(w->next_name, LV_LABEL_LONG_MODE_DOTS);

    w->position = ui_label(parent, &lv_font_montserrat_18, THEME_TEXT);
    lv_obj_set_style_text_opa(w->position, WHEEL_NEIGHBOUR_OPA, 0);
    lv_obj_align(w->position, LV_ALIGN_BOTTOM_MID, 0, -4);

    show(w, true);
}

void ui_wheel_set_count(ui_wheel_t *w, int count)
{
    w->count = count;
    if (w->index >= count) {
        w->index = 0;
    }
    show(w, true);
}

void ui_wheel_move(ui_wheel_t *w, int dir)
{
    if (w->count < 2) {
        return;
    }
    w->index = wrap(w, w->index + dir);
    show(w, true);

    // 新的一項從移動的方向滑進來
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, w->box);
    lv_anim_set_exec_cb(&a, slide_cb);
    lv_anim_set_values(&a, dir * WHEEL_SLIDE_PX, 0);
    lv_anim_set_duration(&a, WHEEL_SLIDE_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

void ui_wheel_refresh(ui_wheel_t *w)
{
    show(w, false);
}
