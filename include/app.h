#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct {
    int prio;
    size_t state_size;      // 前景 app 的 state 大小（bytes），背景 app 是 0
    const char *name;
    const char *icon;
    void (*init)(void *scr);
    void (*run)(void *scr);
    void (*exit)(void *scr);
} app_t;

#define APP_INST_CREATE(_inst, _name, _icon, _init, _run, _exit, _prio, _state_size) \
    static void _init(void *); \
    static void _run(void *); \
    static const app_t _inst = { \
        .prio = _prio , \
        .state_size = _state_size, \
        .name = #_name, \
        .icon = _icon, \
        .init = (void (*)(void *))_init, \
        .run = (void (*)(void *))_run, \
        .exit = (void (*)(void *))_exit, \
    }

#define APP_REGISTER(_name, _icon, _init, _run, _exit, _prio, _state_size) \
    APP_INST_CREATE(app_inst, _name, _icon, _init, _run, _exit, _prio, _state_size); \
    int app_list_insert(const app_t *app); \
    static int app_register(void) \
    { \
        const app_t *app = &app_inst; \
        app_list_insert(app); \
        LOG_INF("Registered app: %s with priority %d", app->name, app->prio); \
        return 0; \
    } \
    SYS_INIT(app_register, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY)

/*
 * 前景 app 的狀態都放在這塊共用記憶體（CONFIG_APP_STATE_SIZE，DRAM region 1）。
 * 同一時間只會開一個前景 app，所以大家輪流用：框架在呼叫 init() 之前把它清成 0，
 * app 關閉後就交給下一個 app。用法見 fg_app.h 的 APP_FG_REGISTER。
 */
extern uint8_t app_state_mem[];
