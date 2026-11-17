#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "ui.h"

#define UI_LABEL_FMT_MAX 64

static lv_style_t style_item;
static lv_style_t style_item_focused;
static lv_style_t style_tab;
static lv_style_t style_tab_focused;
static bool styles_inited;

static void styles_init(void)
{
    if (styles_inited) {
        return;
    }
    styles_inited = true;

    lv_style_init(&style_item);
    lv_style_set_width(&style_item, LV_PCT(100));
    lv_style_set_height(&style_item, 32);
    lv_style_set_radius(&style_item, 10);
    lv_style_set_pad_hor(&style_item, 10);
    lv_style_set_pad_column(&style_item, 8);
    lv_style_set_bg_opa(&style_item, LV_OPA_TRANSP);
    lv_style_set_text_color(&style_item, lv_color_hex(THEME_TEXT));
    lv_style_set_text_font(&style_item, &lv_font_montserrat_14);

    lv_style_init(&style_item_focused);
    lv_style_set_bg_opa(&style_item_focused, LV_OPA_COVER);
    lv_style_set_text_color(&style_item_focused, lv_color_hex(THEME_BG));

    lv_style_init(&style_tab);
    lv_style_set_size(&style_tab, 90, UI_TABS_BOTTOM - UI_TABS_Y);
    lv_style_set_radius(&style_tab, LV_RADIUS_CIRCLE);
    lv_style_set_border_color(&style_tab, lv_color_hex(THEME_TEXT_DIM));
    lv_style_set_border_width(&style_tab, 1);
    lv_style_set_bg_opa(&style_tab, LV_OPA_TRANSP);
    lv_style_set_text_color(&style_tab, lv_color_hex(THEME_TEXT_DIM));
    lv_style_set_text_font(&style_tab, &lv_font_montserrat_14);

    lv_style_init(&style_tab_focused);
    lv_style_set_bg_opa(&style_tab_focused, LV_OPA_COVER);
    lv_style_set_border_width(&style_tab_focused, 0);
    lv_style_set_text_color(&style_tab_focused, lv_color_hex(THEME_BG));
}

void ui_screen_init(lv_obj_t *scr)
{
    lv_obj_set_style_bg_color(scr, lv_color_hex(THEME_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_scrollable(scr, false);
}

lv_obj_t *ui_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, "");
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    return label;
}

void ui_label_set_fmt(lv_obj_t *label, const char *fmt, ...)
{
    char buf[UI_LABEL_FMT_MAX];
    va_list args;

    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    if (strcmp(lv_label_get_text(label), buf) != 0) {
        lv_label_set_text(label, buf);
    }
}

lv_obj_t *ui_title(lv_obj_t *scr, const char *text)
{
    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, text);
    lv_obj_set_width(title, 220);
    lv_label_set_long_mode(title, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(THEME_TEXT_DIM), 0);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);
    return title;
}

lv_obj_t *ui_list_create(lv_obj_t *scr)
{
    styles_init();

    lv_obj_t *list = lv_obj_create(scr);
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, LV_PCT(100), UI_SCREEN_SIZE - 34);
    lv_obj_align(list, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_hor(list, 8, 0);
    lv_obj_set_style_pad_bottom(list, 8, 0);
    lv_obj_set_style_pad_row(list, 2, 0);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
    return list;
}

lv_obj_t *ui_list_item(lv_obj_t *list, const char *icon, const char *text, const char *detail,
                       lv_color_t accent)
{
    lv_obj_t *item = lv_obj_create(list);
    lv_obj_remove_style_all(item);
    lv_obj_add_style(item, &style_item, 0);
    lv_obj_add_style(item, &style_item_focused, LV_STATE_FOCUSED);
    lv_obj_set_style_bg_color(item, accent, 0);
    lv_obj_set_scroll_on_focus(item, true);
    lv_obj_set_scrollable(item, false);
    lv_obj_set_flex_flow(item, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(item, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *label = lv_label_create(item);
    lv_label_set_text(label, icon);

    label = lv_label_create(item);
    lv_label_set_text(label, text);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_flex_grow(label, 1);

    if (detail != NULL) {
        label = lv_label_create(item);
        lv_label_set_text(label, detail);
    }

    lv_group_add_obj(lv_group_get_default(), item);
    return item;
}

lv_obj_t *ui_list_row(lv_obj_t *list, const char *icon, const char *text, lv_color_t accent)
{
    lv_obj_t *item = ui_list_item(list, icon, text, "-", accent);
    return lv_obj_get_child(item, 2);
}

static void strdup_free_cb(lv_event_t *e)
{
    lv_free(lv_event_get_user_data(e));
}

const char *ui_obj_strdup(lv_obj_t *obj, const char *text)
{
    char *copy = lv_strdup(text);
    if (copy != NULL) {
        lv_obj_add_event_cb(obj, strdup_free_cb, LV_EVENT_DELETE, copy);
    }
    return copy;
}

lv_obj_t *ui_list_message(lv_obj_t *list, const char *text)
{
    lv_obj_t *label = lv_label_create(list);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, lv_color_hex(THEME_TEXT_DIM), 0);
    lv_obj_set_style_pad_top(label, 40, 0);
    lv_obj_set_width(label, LV_PCT(100));
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    return label;
}

lv_obj_t *ui_tabs(lv_obj_t *scr, const char *const names[], int count, lv_color_t accent,
                  lv_event_cb_t focused_cb, lv_event_cb_t clicked_cb)
{
    styles_init();

    lv_obj_t *tabs = lv_obj_create(scr);
    lv_obj_remove_style_all(tabs);
    lv_obj_set_size(tabs, LV_PCT(100), UI_TABS_BOTTOM - UI_TABS_Y);
    lv_obj_align(tabs, LV_ALIGN_TOP_MID, 0, UI_TABS_Y);
    lv_obj_set_flex_flow(tabs, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(tabs, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(tabs, 12, 0);
    lv_obj_set_scrollable(tabs, false);

    for (int i = 0; i < count; i++) {
        lv_obj_t *tab = lv_obj_create(tabs);
        lv_obj_remove_style_all(tab);
        lv_obj_add_style(tab, &style_tab, 0);
        lv_obj_add_style(tab, &style_tab_focused, LV_STATE_FOCUSED);
        lv_obj_set_style_bg_color(tab, accent, 0);
        lv_obj_set_scrollable(tab, false);

        lv_obj_t *label = lv_label_create(tab);
        lv_label_set_text(label, names[i]);
        lv_obj_center(label);

        lv_obj_add_event_cb(tab, focused_cb, LV_EVENT_FOCUSED, (void *)(intptr_t)i);
        lv_obj_add_event_cb(tab, clicked_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_group_add_obj(lv_group_get_default(), tab);
    }
    return tabs;
}

lv_obj_t *ui_tap_area(lv_obj_t *scr, lv_event_cb_t cb, void *user_data)
{
    lv_obj_t *area = lv_obj_create(scr);
    lv_obj_remove_style_all(area);
    lv_obj_set_size(area, LV_PCT(100), LV_PCT(100));
    lv_obj_set_scrollable(area, false);
    lv_obj_add_event_cb(area, cb, LV_EVENT_CLICKED, user_data);
    lv_group_add_obj(lv_group_get_default(), area);
    return area;
}

void ui_group_solo(lv_obj_t *obj)
{
    lv_group_t *g = lv_group_get_default();
    lv_group_remove_all_objs(g);
    lv_group_add_obj(g, obj);
}

void ui_group_add_children(lv_obj_t *parent)
{
    lv_group_t *g = lv_group_get_default();
    uint32_t count = lv_obj_get_child_count(parent);
    for (uint32_t i = 0; i < count; i++) {
        lv_obj_t *child = lv_obj_get_child(parent, i);
        if (lv_obj_is_clickable(child)) {
            lv_group_add_obj(g, child);
        }
    }
}
