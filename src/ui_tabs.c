#include "ui_tabs.h"
#include "ui.h"
#include "nav.h"
#include "prefs.h"

#define UI_TAB_PAD_HOR      14
#define UI_TAB_MIN_WIDTH    90
#define UI_TAB_GAP          8

// 同一時間只有一個前景 app，狀態放這裡就好；分頁籤被刪掉（app 關閉）時清掉
static struct {
    const ui_tabs_cfg_t *cfg;
    lv_obj_t *tabs;
    int current;
    int saved;
    bool creating;
} state;

static lv_style_t style_tab;
static lv_style_t style_tab_focused;
static bool styles_inited;

static void styles_init(void)
{
    if (styles_inited) {
        return;
    }
    styles_inited = true;

    lv_style_init(&style_tab);
    lv_style_set_height(&style_tab, UI_TABS_BOTTOM - UI_TABS_Y);
    lv_style_set_width(&style_tab, LV_SIZE_CONTENT);
    lv_style_set_min_width(&style_tab, UI_TAB_MIN_WIDTH);
    lv_style_set_pad_hor(&style_tab, UI_TAB_PAD_HOR);
    lv_style_set_radius(&style_tab, LV_RADIUS_CIRCLE);
    lv_style_set_border_color(&style_tab, lv_color_hex(THEME_TEXT_DIM));
    lv_style_set_border_width(&style_tab, 1);
    lv_style_set_bg_opa(&style_tab, LV_OPA_TRANSP);
    lv_style_set_text_color(&style_tab, lv_color_hex(THEME_TEXT));
    lv_style_set_text_font(&style_tab, &lv_font_montserrat_18);

    lv_style_init(&style_tab_focused);
    lv_style_set_bg_opa(&style_tab_focused, LV_OPA_COVER);
    lv_style_set_border_width(&style_tab_focused, 0);
    lv_style_set_text_color(&style_tab_focused, lv_color_hex(THEME_BG));
}

static void focused_cb(lv_event_t *e)
{
    int index = (int)(intptr_t)lv_event_get_user_data(e);

    // 建立時加進 group 會自動 focus 第一個，先不理，建好再顯示記住的那一頁
    if (state.creating || index == state.current) {
        return;
    }
    state.current = index;
    state.cfg->show(index);
}

static void clicked_cb(lv_event_t *e)
{
    state.cfg->action((int)(intptr_t)lv_event_get_user_data(e), false);
}

// 敲一下跟 OK 一樣；上下捲動頁面。next / prev / OK 照預設在分頁籤之間移動、點目前那個，
// 左右沒處理會變成 prev / next，也就是切分頁
static bool nav_cb(nav_cmd_t cmd)
{
    if (state.cfg == NULL) {
        return false;
    }
    switch (cmd) {
    case NAV_TAP:
        state.cfg->action(state.current, true);
        return true;
    case NAV_UP:
    case NAV_DOWN:
        if (state.cfg->scroll != NULL) {
            state.cfg->scroll(state.current, cmd == NAV_UP ? -1 : 1);
        }
        return true;
    default:
        return false;
    }
}

static void delete_cb(lv_event_t *e)
{
    if (state.cfg->pref_key != NULL && state.current != state.saved) {
        uint8_t saved = state.current;
        prefs_save(state.cfg->pref_key, &saved, sizeof(saved));
    }
    state.cfg = NULL;
    state.tabs = NULL;
}

lv_obj_t *ui_tabs_create(lv_obj_t *scr, const ui_tabs_cfg_t *cfg)
{
    styles_init();

    state.cfg = cfg;
    state.current = -1;
    state.saved = 0;
    if (cfg->pref_key != NULL) {
        uint8_t saved;
        if (prefs_load(cfg->pref_key, &saved, sizeof(saved)) == sizeof(saved) && saved < cfg->count) {
            state.saved = saved;
        }
    }

    lv_obj_t *tabs = lv_obj_create(scr);
    lv_obj_remove_style_all(tabs);
    lv_obj_set_size(tabs, LV_PCT(100), UI_TABS_BOTTOM - UI_TABS_Y);
    lv_obj_align(tabs, LV_ALIGN_TOP_MID, 0, UI_TABS_Y);
    lv_obj_set_flex_flow(tabs, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(tabs, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(tabs, UI_TAB_GAP, 0);
    lv_obj_set_scrollable(tabs, false);
    lv_obj_add_event_cb(tabs, delete_cb, LV_EVENT_DELETE, NULL);
    state.tabs = tabs;

    state.creating = true;
    for (int i = 0; i < cfg->count; i++) {
        lv_obj_t *tab = lv_obj_create(tabs);
        lv_obj_remove_style_all(tab);
        lv_obj_add_style(tab, &style_tab, 0);
        lv_obj_add_style(tab, &style_tab_focused, LV_STATE_FOCUSED);
        lv_obj_set_style_bg_color(tab, lv_color_hex(cfg->accent), 0);
        lv_obj_set_scrollable(tab, false);

        lv_obj_t *label = lv_label_create(tab);
        lv_label_set_text(label, cfg->names[i]);
        lv_obj_center(label);

        lv_obj_add_event_cb(tab, focused_cb, LV_EVENT_FOCUSED, (void *)(intptr_t)i);
        lv_obj_add_event_cb(tab, clicked_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_group_add_obj(lv_group_get_default(), tab);
    }
    lv_group_focus_obj(lv_obj_get_child(tabs, state.saved));
    state.creating = false;

    state.current = state.saved;
    cfg->show(state.current);
    nav_set_handler(nav_cb);
    return tabs;
}

int ui_tabs_current(void)
{
    return state.current;
}
