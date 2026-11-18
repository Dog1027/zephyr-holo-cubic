#pragma once

#include <lvgl.h>
#include <stdbool.h>

/*
 * Mode tabs at the top of an app with several pages (Dice, Clock, System). Every app that uses
 * them behaves the same:
 *   next / prev, left / right   switch to the next / previous tab
 *   up / down                   scroll the current page (when it has scroll), never switch tabs
 *   OK, tap                     the current page's action (roll the dice, refresh the weather...)
 * The last tab is remembered when pref_key is set. The tabs take the app's nav handler, so do
 * not call nav_set_handler() as well. Only the tabs are in the default group: a page must not
 * add its own objects (for read-only lists use ui_wheel.h).
 */

#define UI_TABS_Y       12      // the tab row: from here
#define UI_TABS_BOTTOM  40      // to here, put the content below

typedef struct {
    const char *const *names;
    uint8_t count;
    uint32_t accent;            // 0xRRGGBB of the current tab
    const char *pref_key;       // prefs key to remember the last tab, NULL to always open the first
    void (*show)(int index);    // tab index became current, also once at start: show its page
    void (*action)(int index, bool tap);    // OK (tap false) or a tap on the cube (tap true) on tab index
    void (*scroll)(int index, int dir); // up (dir -1) / down (+1) on tab index, NULL if the pages do not scroll
} ui_tabs_cfg_t;

// cfg must stay valid while the app is open (make it static const)
lv_obj_t *ui_tabs_create(lv_obj_t *scr, const ui_tabs_cfg_t *cfg);

// Index of the current tab
int ui_tabs_current(void);
