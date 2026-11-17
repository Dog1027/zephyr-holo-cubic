#pragma once

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <lvgl.h>
#include "app.h"
#include "icons.h"

/*
 * Register a foreground app.
 * _prio:    order in the menu, 0 is reserved for the background app
 * _icon:    ICON_* (icons.h) shown in the menu, NULL for a default icon
 * _state_t: struct with everything the app keeps while it is open, by convention
 *           `typedef struct { ... } self_t;` in the app's file; app_no_state_t for none.
 *
 * The state lives in app_state_mem, shared by all foreground apps (only one is open at a time).
 * The macro defines `self`, a pointer to it: write self->label instead of a static variable.
 * It is zeroed right before init() and handed to the next app after exit(), so:
 *   - set non-zero defaults in init()
 *   - nothing may use it after exit(): no timers, callbacks or other threads holding a pointer
 * Data that must outlive the app (a running timer, LVGL styles initialized once) stays in
 * ordinary static variables, with a comment saying why.
 */
#define APP_FG_REGISTER(_name, _icon, _init, _run, _exit, _prio, _state_t) \
    BUILD_ASSERT((_prio) != 0, "Foreground app priority cannot be 0"); \
    BUILD_ASSERT(sizeof(_state_t) <= CONFIG_APP_STATE_SIZE, \
                 #_name " state does not fit, raise CONFIG_APP_STATE_SIZE"); \
    LOG_MODULE_REGISTER(_name, CONFIG_CUBE_LOG_LEVEL); \
    static _state_t *const self __maybe_unused = (_state_t *)app_state_mem; \
    static void _exit(void *); \
    APP_REGISTER(_name, _icon, _init, _run, _exit, _prio, sizeof(_state_t))

// State type for an app that keeps nothing
typedef struct {
} app_no_state_t;
