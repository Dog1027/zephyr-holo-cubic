#pragma once

#include <lvgl.h>
#include "theme.h"

#define UI_SCREEN_SIZE  240
#define UI_TABS_Y       12      // ui_tabs() row: from here
#define UI_TABS_BOTTOM  40      // to here, put the content below

// Black background, no scrolling
void ui_screen_init(lv_obj_t *scr);

// Label with a font and a 0xRRGGBB color, empty text
lv_obj_t *ui_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color);

// printf into a label, only when the text changes (setting the same text redraws it anyway)
void ui_label_set_fmt(lv_obj_t *label, const char *fmt, ...);

// Small grey title at the top of the screen
lv_obj_t *ui_title(lv_obj_t *scr, const char *text);

// Vertical list below the title, items scroll into view when highlighted
lv_obj_t *ui_list_create(lv_obj_t *scr);

// List row: child 0 is the icon, child 1 the text, child 2 the optional detail.
// The row is added to the default group and gets the accent color when highlighted.
lv_obj_t *ui_list_item(lv_obj_t *list, const char *icon, const char *text, const char *detail,
                       lv_color_t accent);

// List row with a value on the right, "-" at first. Returns the value label (set it with
// ui_label_set_fmt); the row itself is its parent
lv_obj_t *ui_list_row(lv_obj_t *list, const char *icon, const char *text, lv_color_t accent);

// Copy of text that is freed together with obj. Use it instead of reading a label back:
// LV_LABEL_LONG_MODE_DOTS overwrites the end of the label's own text with "..."
const char *ui_obj_strdup(lv_obj_t *obj, const char *text);

// Grey centered text inside a list, e.g. "Empty folder"
lv_obj_t *ui_list_message(lv_obj_t *list, const char *text);

// Mode tabs at the top of an app with several modes (Dice, Clock). next / prev move between them,
// OK clicks the highlighted one. Both callbacks get the tab's index as user data
// (lv_event_get_user_data). Adding the first tab to the group focuses it, so focused_cb runs
// for tab 0 inside this call: create what it shows first. Tab i is lv_obj_get_child(tabs, i).
lv_obj_t *ui_tabs(lv_obj_t *scr, const char *const names[], int count, lv_color_t accent,
                  lv_event_cb_t focused_cb, lv_event_cb_t clicked_cb);

// Invisible full screen object in the default group, receives OK (ui ok) when nothing else is selectable
lv_obj_t *ui_tap_area(lv_obj_t *scr, lv_event_cb_t cb, void *user_data);

// Keep only obj in the default group, so next / prev cannot move the highlight away
void ui_group_solo(lv_obj_t *obj);

// Put the clickable children of parent back into the default group
void ui_group_add_children(lv_obj_t *parent);
