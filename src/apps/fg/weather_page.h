#pragma once

#include "ui.h"
#include "weather.h"

#define WEATHER_ACCENT_COLOR    THEME_YELLOW

typedef struct {
    lv_obj_t *icon;
    lv_obj_t *name;
    lv_obj_t *temp;
} weather_day_view_t;

// Clock app 的天氣分頁，放在 app 的 self_t 裡，不用初始化
typedef struct {
    lv_obj_t *area;
    lv_obj_t *icon;
    lv_obj_t *temp;
    lv_obj_t *desc;
    lv_obj_t *detail;
    lv_obj_t *status;
    // 還沒有資料時的等待畫面：轉圈、status 和下面一行小字
    lv_obj_t *spinner;
    lv_obj_t *hint;
    weather_day_view_t days[WEATHER_DAYS];
    weather_t data;
    uint32_t shown_generation;
    int shown_state;
    int64_t last_status;
} weather_page_t;

// 在 scr（整個畫面大小的容器）裡建立，內容在 UI_TABS_BOTTOM 以下
void weather_page_create(weather_page_t *v, lv_obj_t *scr);

// 顯示這一頁時每次 run 呼叫：資料太舊會請背景重抓，有新資料就重畫
void weather_page_run(weather_page_t *v);

// 馬上重抓
void weather_page_refresh(void);
