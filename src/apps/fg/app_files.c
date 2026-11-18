#include "fg_app.h"
#include "file.h"
#include "ui.h"
#include "nav.h"
#include <zephyr/fs/fs.h>
#include <string.h>
#include <stdio.h>

#define FILES_ROOT          FILE_ROOT
#define FILES_PATH_MAX      128
#define FILES_MAX_ENTRIES   24  // 每個 item 都佔 LVGL 記憶體，太多會耗盡 pool
#define FILES_ACCENT_COLOR  THEME_ORANGE

/*
 * 瀏覽 SD 卡。清單只有名稱（太長變 "..."），點檔案才顯示完整名稱和大小，OK 或敲一下回到清單。
 */

typedef struct {
    lv_obj_t *path_label;
    lv_obj_t *list;
    lv_obj_t *info;         // 檔案資訊，蓋在清單上面
    lv_obj_t *info_name;
    lv_obj_t *info_size;
    lv_obj_t *info_item;    // 點開資訊的那一列，關掉後 highlight 回到它
    lv_obj_t *pending_info;
    const char *pending_name;
    bool pending_close;
    char cur_path[FILES_PATH_MAX];
    char pending_path[FILES_PATH_MAX];
    bool pending;
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

// 檔案的列：名稱另存一份（label 太長會被改成 "..."），大小放在列的 user data
static void file_clicked_cb(lv_event_t *e)
{
    // 不在列自己的事件裡改 group，交給 run
    self->pending_info = lv_event_get_target_obj(e);
    self->pending_name = lv_event_get_user_data(e);
}

static void info_clicked_cb(lv_event_t *e)
{
    self->pending_close = true;
}

static void info_show(lv_obj_t *item, const char *name)
{
    char size[24];

    lv_label_set_text(self->info_name, name);
    format_size(size, sizeof(size), (size_t)(uintptr_t)lv_obj_get_user_data(item));
    lv_label_set_text(self->info_size, size);
    self->info_item = item;
    lv_obj_set_hidden(self->info, false);
    ui_group_solo(self->info);
}

static void info_close(void)
{
    lv_obj_set_hidden(self->info, true);
    lv_group_remove_all_objs(lv_group_get_default());
    ui_group_add_children(self->list);
    lv_group_focus_obj(self->info_item);
}

// 資訊開著時，敲一下也是關掉
static bool nav_cb(nav_cmd_t cmd)
{
    if (cmd == NAV_TAP && !lv_obj_is_hidden(self->info)) {
        self->pending_close = true;
        return true;
    }
    return false;
}

static void item_clicked_cb(lv_event_t *e)
{
    const char *name = lv_event_get_user_data(e);

    // 不能在 item 自己的事件裡刪掉列表，交給 run 在下一幀重建
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

// name 為 NULL 表示「上一層」
static void dir_item_create(const char *icon, const char *name)
{
    lv_obj_t *item = ui_list_item(self->list, icon, name ? name : "..", NULL, lv_color_hex(FILES_ACCENT_COLOR));

    // 另存一份完整名稱，label 的文字太長時會被改成 "..."
    const char *text = name ? ui_obj_strdup(item, name) : NULL;
    if (name != NULL && text == NULL) {
        return;
    }
    lv_obj_add_event_cb(item, item_clicked_cb, LV_EVENT_CLICKED, (void *)text);
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
            lv_obj_t *item = ui_list_item(self->list, LV_SYMBOL_FILE, entry.name, NULL,
                                          lv_color_hex(FILES_ACCENT_COLOR));
            const char *name = ui_obj_strdup(item, entry.name);
            if (name != NULL) {
                lv_obj_set_user_data(item, (void *)(uintptr_t)entry.size);
                lv_obj_add_event_cb(item, file_clicked_cb, LV_EVENT_CLICKED, (void *)name);
            }
        }
        count++;
    }

    fs_closedir(&dir);
    return ret < 0 ? ret : count;
}

static void list_refresh(void)
{
    lv_obj_clean(self->list);
    lv_label_set_text(self->path_label, self->cur_path);

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
}

static void app_files_init(void *scr)
{
    ui_screen_init(scr);
    self->path_label = ui_title(scr, FILES_ROOT);
    self->list = ui_list_create(scr);

    // 檔案資訊：整個畫面，蓋住標題和清單
    self->info = lv_obj_create(scr);
    lv_obj_remove_style_all(self->info);
    lv_obj_set_size(self->info, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(self->info, lv_color_hex(THEME_BG), 0);
    lv_obj_set_style_bg_opa(self->info, LV_OPA_COVER, 0);
    lv_obj_set_scrollable(self->info, false);
    lv_obj_add_event_cb(self->info, info_clicked_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_set_hidden(self->info, true);

    lv_obj_t *icon = ui_label(self->info, &lv_font_montserrat_28, FILES_ACCENT_COLOR);
    lv_label_set_text(icon, LV_SYMBOL_FILE);
    lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 28);

    // 完整名稱，太長就換行
    self->info_name = ui_label(self->info, &lv_font_montserrat_18, THEME_TEXT);
    lv_obj_set_width(self->info_name, UI_SCREEN_SIZE - 32);
    lv_label_set_long_mode(self->info_name, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(self->info_name, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(self->info_name, LV_ALIGN_TOP_MID, 0, 72);

    self->info_size = ui_label(self->info, &lv_font_montserrat_28, THEME_TEXT);
    lv_obj_align(self->info_size, LV_ALIGN_BOTTOM_MID, 0, -40);

    strcpy(self->cur_path, FILES_ROOT);
    list_refresh();
    nav_set_handler(nav_cb);
}

static void app_files_run(void *scr)
{
    if (self->pending_info != NULL) {
        info_show(self->pending_info, self->pending_name);
        self->pending_info = NULL;
    }
    if (self->pending_close) {
        self->pending_close = false;
        info_close();
    }
    if (self->pending) {
        self->pending = false;
        strcpy(self->cur_path, self->pending_path);
        list_refresh();
    }
}

static void app_files_exit(void *scr)
{
}
