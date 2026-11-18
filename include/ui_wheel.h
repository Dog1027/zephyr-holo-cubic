#pragma once

#include <lvgl.h>
#include <stdbool.h>

/*
 * One item at a time, for read-only lists on the small screen (System): the current item's name
 * and a big value in the middle, an optional bar, and the names of the previous and next items
 * faded above and below. Moving wraps around at both ends.
 * Values are not stored: the app's get callback fills an item when it is shown or refreshed.
 */

#define UI_WHEEL_NAME_MAX   32
#define UI_WHEEL_VALUE_MAX  48

typedef struct {
    char name[UI_WHEEL_NAME_MAX];
    char value[UI_WHEEL_VALUE_MAX];
    int percent;        // 0-100 draws a bar under the value, -1 for none
    lv_obj_t *extra;    // app object shown instead of the bar (child of ui_wheel_t.box), or NULL
} ui_wheel_item_t;

// Fill item index; name only matters for the neighbours, so it can skip the rest when name_only
typedef void (*ui_wheel_get_t)(int index, ui_wheel_item_t *item, bool name_only);

// Lives in the app's self_t, no initialisation needed
typedef struct {
    lv_obj_t *box;      // name, value, bar: slides when moving; put ui_wheel_item_t.extra objects here
    lv_obj_t *prev_name;
    lv_obj_t *name;
    lv_obj_t *value;
    lv_obj_t *bar;
    lv_obj_t *next_name;
    lv_obj_t *position;
    lv_obj_t *shown_extra;
    ui_wheel_get_t get;
    int count;
    int index;
} ui_wheel_t;

// In parent (a full screen container), using the area below top (e.g. UI_TABS_BOTTOM)
void ui_wheel_create(ui_wheel_t *w, lv_obj_t *parent, int32_t top, uint32_t accent, ui_wheel_get_t get, int count);

// The number of items changed (e.g. a scan finished); keeps the current index when it still exists
void ui_wheel_set_count(ui_wheel_t *w, int count);

// Show the previous (dir -1) or next (+1) item, wrapping around
void ui_wheel_move(ui_wheel_t *w, int dir);

// Read the current item again (call it periodically for live values)
void ui_wheel_refresh(ui_wheel_t *w);
