#include "fg_app.h"
#include "file.h"
#include "ui.h"
#include "ui_tabs.h"
#include "share_page.h"
#include <zephyr/fs/fs.h>
#include <string.h>
#include <stdio.h>

/*
 * SD 卡的檔案，兩個分頁（ui_tabs.h）：
 *   FILES：瀏覽。清單只有名稱（太長變 "..."），上下移動選取，OK 或敲一下打開資料夾，
 *          或顯示檔案的完整名稱和大小（再按一次回到清單）。在子資料夾時第一列是上一層和目前路徑。
 *   SHARE：網頁版的檔案管理（share_page.c），只有這一頁顯示時才提供服務。OK 換一組 PIN。
 */

#define FILES_ROOT          FILE_ROOT
#define FILES_PATH_MAX      128
#define FILES_MAX_ENTRIES   24  // 每個 item 都佔 LVGL 記憶體，太多會耗盡 pool
#define FILES_ACCENT_COLOR  THEME_ORANGE

enum { MODE_FILES, MODE_SHARE, MODE_COUNT };

typedef struct {
    lv_obj_t *pages[MODE_COUNT];    // 每個分頁一個容器，只顯示目前那個
    int mode;
    lv_obj_t *list;
    lv_obj_t *selected;     // 清單裡 highlight 的那一列（列不在 group 裡，自己記）
    lv_obj_t *info;         // 檔案資訊，蓋在清單上面
    lv_obj_t *info_name;
    lv_obj_t *info_size;
    char cur_path[FILES_PATH_MAX];
    char pending_path[FILES_PATH_MAX];
    bool pending;
    share_page_t share;
} self_t;

APP_FG_REGISTER(Files, ICON_FILES, app_files_init, app_files_run, app_files_exit, 6, self_t);

// 一位小數，不用 %f (picolibc 可能沒有浮點 printf)
static void format_size(char *buf, size_t len, size_t size)
{
    static const char *const units[] = {"KB", "MB", "GB"};
    uint64_t tenths;
    int unit = 0;

    if (size < 1024) {
        snprintf(buf, len, "%u B", (unsigned int)size);
        return;
    }
    tenths = (uint64_t)size * 10 / 1024;
    while (tenths >= 10240 && unit < (int)ARRAY_SIZE(units) - 1) {
        tenths /= 1024;
        unit++;
    }
    snprintf(buf, len, "%u.%u %s", (unsigned int)(tenths / 10), (unsigned int)(tenths % 10), units[unit]);
}

/* ---------- FILES ---------- */

static bool info_shown(void)
{
    return !lv_obj_is_hidden(self->info);
}

// 檔案的列：名稱另存一份（label 太長會被改成 "..."），大小放在列的 user data
static void file_clicked_cb(lv_event_t *e)
{
    char size[24];

    lv_label_set_text(self->info_name, lv_event_get_user_data(e));
    format_size(size, sizeof(size), (size_t)(uintptr_t)lv_obj_get_user_data(lv_event_get_target_obj(e)));
    lv_label_set_text(self->info_size, size);
    lv_obj_set_hidden(self->info, false);
}

static void dir_clicked_cb(lv_event_t *e)
{
    const char *name = lv_event_get_user_data(e);

    // 不能在列自己的事件裡刪掉清單，交給 run 在下一幀重建
    if (name == NULL) {
        strcpy(self->pending_path, self->cur_path);
        char *slash = strrchr(self->pending_path, '/');
        if (slash != NULL && slash != self->pending_path) {
            *slash = '\0';
        }
    }
    else {
        // 先組在 stack 上：compiler 看不出 self 的兩個欄位不重疊（-Wrestrict）
        char path[FILES_PATH_MAX];
        int len = snprintf(path, sizeof(path), "%s/%s", self->cur_path, name);
        if (len < 0 || len >= (int)sizeof(path)) {
            LOG_WRN("Path too long: %s/%s", self->cur_path, name);
            return;
        }
        strcpy(self->pending_path, path);
    }
    self->pending = true;
}

// name 為 NULL 表示上一層，列上顯示目前的路徑
static void dir_item_create(const char *icon, const char *name)
{
    lv_obj_t *item = ui_list_item(self->list, icon, name ? name : self->cur_path, NULL,
                                  lv_color_hex(FILES_ACCENT_COLOR));

    // 另存一份完整名稱，label 的文字太長時會被改成 "..."
    const char *text = name ? ui_obj_strdup(item, name) : NULL;
    if (name != NULL && text == NULL) {
        return;
    }
    lv_obj_add_event_cb(item, dir_clicked_cb, LV_EVENT_CLICKED, (void *)text);
}

static void file_item_create(const char *name, size_t size)
{
    lv_obj_t *item = ui_list_item(self->list, LV_SYMBOL_FILE, name, NULL, lv_color_hex(FILES_ACCENT_COLOR));
    const char *text = ui_obj_strdup(item, name);

    if (text != NULL) {
        lv_obj_set_user_data(item, (void *)(uintptr_t)size);
        lv_obj_add_event_cb(item, file_clicked_cb, LV_EVENT_CLICKED, (void *)text);
    }
}

// 讀一次目錄，只建立目錄 (want_dir) 或只建立檔案，用兩次呼叫讓目錄排在前面
static int list_entries(bool want_dir, int count)
{
    struct fs_dirent entry;
    struct fs_dir_t dir;
    int ret;

    fs_dir_t_init(&dir);
    ret = fs_opendir(&dir, self->cur_path);
    if (ret < 0) {
        LOG_ERR("Failed to open %s (%d)", self->cur_path, ret);
        return ret;
    }

    while (count < FILES_MAX_ENTRIES) {
        ret = fs_readdir(&dir, &entry);
        if (ret < 0 || entry.name[0] == '\0') {
            break;
        }

        bool is_dir = entry.type == FS_DIR_ENTRY_DIR;
        if (is_dir != want_dir) {
            continue;
        }
        if (is_dir) {
            dir_item_create(LV_SYMBOL_DIRECTORY, entry.name);
        }
        else {
            file_item_create(entry.name, entry.size);
        }
        count++;
    }

    fs_closedir(&dir);
    return ret < 0 ? ret : count;
}

static void list_refresh(void)
{
    lv_obj_clean(self->list);
    self->selected = NULL;

    if (strcmp(self->cur_path, FILES_ROOT) != 0) {
        dir_item_create(LV_SYMBOL_UP, NULL);
    }

    int count = list_entries(true, 0);
    if (count >= 0) {
        count = list_entries(false, count);
    }

    if (count < 0) {
        ui_list_message(self->list, "Cannot read SD card");
    }
    else if (count == 0) {
        ui_list_message(self->list, "Empty folder");
    }
    else if (count >= FILES_MAX_ENTRIES) {
        ui_list_message(self->list, "...");
    }

    // 分頁籤佔著 next / prev，列不進 group，highlight 自己管
    ui_group_remove_children(self->list);
    self->selected = ui_list_step(self->list, NULL, 1);
}

static void files_page_create(lv_obj_t *scr)
{
    self->list = ui_list_create(scr);
    lv_obj_set_height(self->list, UI_SCREEN_SIZE - UI_TABS_BOTTOM - 4);

    // 檔案資訊：分頁籤以下整塊，蓋住清單
    self->info = lv_obj_create(scr);
    lv_obj_remove_style_all(self->info);
    lv_obj_set_size(self->info, LV_PCT(100), UI_SCREEN_SIZE - UI_TABS_BOTTOM);
    lv_obj_align(self->info, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(self->info, lv_color_hex(THEME_BG), 0);
    lv_obj_set_style_bg_opa(self->info, LV_OPA_COVER, 0);
    lv_obj_set_scrollable(self->info, false);
    lv_obj_set_clickable(self->info, false);
    lv_obj_set_hidden(self->info, true);

    lv_obj_t *icon = ui_label(self->info, &lv_font_montserrat_28, FILES_ACCENT_COLOR);
    lv_label_set_text(icon, LV_SYMBOL_FILE);
    lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 10);

    // 完整名稱，太長就換行
    self->info_name = ui_label(self->info, &lv_font_montserrat_18, THEME_TEXT);
    lv_obj_set_width(self->info_name, UI_SCREEN_SIZE - 32);
    lv_label_set_long_mode(self->info_name, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(self->info_name, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(self->info_name, LV_ALIGN_TOP_MID, 0, 50);

    self->info_size = ui_label(self->info, &lv_font_montserrat_28, THEME_TEXT);
    lv_obj_align(self->info_size, LV_ALIGN_BOTTOM_MID, 0, -30);

    strcpy(self->cur_path, FILES_ROOT);
    list_refresh();
}

/* ---------- 分頁 ---------- */

static void tab_show(int mode)
{
    // 離開 SHARE 就停掉 server，回到 FILES 時重讀，看得到剛上傳的檔案
    if (self->mode == MODE_SHARE && mode != MODE_SHARE) {
        share_page_stop();
    }
    self->mode = mode;
    for (int i = 0; i < MODE_COUNT; i++) {
        lv_obj_set_hidden(self->pages[i], i != mode);
    }
    if (mode == MODE_SHARE) {
        share_page_start();
    }
    else {
        lv_obj_set_hidden(self->info, true);
        list_refresh();
    }
}

// FILES：打開選取的那一列，或關掉檔案資訊。SHARE：OK 換 PIN（敲一下不換，碰到方塊不該讓瀏覽器重輸 PIN）
static void tab_action(int mode, bool tap)
{
    if (mode == MODE_SHARE) {
        if (!tap) {
            share_page_new_pin();
        }
        return;
    }
    if (info_shown()) {
        lv_obj_set_hidden(self->info, true);
    }
    else if (self->selected != NULL) {
        lv_obj_send_event(self->selected, LV_EVENT_CLICKED, NULL);
    }
}

static void tab_scroll(int mode, int dir)
{
    if (mode == MODE_FILES && !info_shown()) {
        self->selected = ui_list_step(self->list, self->selected, dir);
    }
}

static lv_obj_t *page_create(lv_obj_t *scr)
{
    lv_obj_t *page = lv_obj_create(scr);
    lv_obj_remove_style_all(page);
    lv_obj_set_size(page, LV_PCT(100), LV_PCT(100));
    lv_obj_set_scrollable(page, false);
    lv_obj_set_clickable(page, false);
    return page;
}

static void app_files_init(void *scr)
{
    ui_screen_init(scr);
    self->mode = MODE_FILES;

    self->pages[MODE_FILES] = page_create(scr);
    files_page_create(self->pages[MODE_FILES]);
    self->pages[MODE_SHARE] = page_create(scr);
    share_page_create(&self->share, self->pages[MODE_SHARE]);

    // 建立時就會呼叫 tab_show，內容要先建好
    static const char *const names[] = {"FILES", "SHARE"};
    static const ui_tabs_cfg_t tabs = {
        .names = names, .count = ARRAY_SIZE(names), .accent = FILES_ACCENT_COLOR,
        .pref_key = "files/mode", .show = tab_show, .action = tab_action, .scroll = tab_scroll,
    };
    ui_tabs_create(scr, &tabs);
}

static void app_files_run(void *scr)
{
    if (self->mode == MODE_SHARE) {
        share_page_run();
        return;
    }
    if (self->pending) {
        self->pending = false;
        strcpy(self->cur_path, self->pending_path);
        list_refresh();
    }
}

static void app_files_exit(void *scr)
{
    if (self->mode == MODE_SHARE) {
        share_page_stop();
    }
}
