#include "fg_app.h"
#include "file.h"
#include "ui.h"
#include <zephyr/fs/fs.h>
#include <string.h>
#include <stdio.h>

#define FILES_ROOT          FILE_ROOT
#define FILES_PATH_MAX      128
#define FILES_MAX_ENTRIES   24  // 每個 item 都佔 LVGL 記憶體，太多會耗盡 pool
#define FILES_ACCENT_COLOR  0xf5a623

// app 開著時的狀態，放在共用的 app_state_mem（fg_app.h），每次打開前清成 0
typedef struct {
    lv_obj_t *path_label;
    lv_obj_t *list;
    char cur_path[FILES_PATH_MAX];
    char pending_path[FILES_PATH_MAX];
    bool pending;
} self_t;

APP_FG_REGISTER(Files, LV_SYMBOL_DIRECTORY, app_files_init, app_files_run, app_files_exit, 6, self_t);

static void format_size(char *buf, size_t len, size_t size)
{
    if (size < 1024) {
        snprintf(buf, len, "%u B", (unsigned int)size);
    }
    else if (size < 1024 * 1024) {
        snprintf(buf, len, "%u KB", (unsigned int)(size / 1024));
    }
    else {
        snprintf(buf, len, "%u MB", (unsigned int)(size / (1024 * 1024)));
    }
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
    char size[16];
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
            format_size(size, sizeof(size), entry.size);
            ui_list_item(self->list, LV_SYMBOL_FILE, entry.name, size, lv_color_hex(FILES_ACCENT_COLOR));
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

    strcpy(self->cur_path, FILES_ROOT);
    list_refresh();
}

static void app_files_run(void *scr)
{
    if (self->pending) {
        self->pending = false;
        strcpy(self->cur_path, self->pending_path);
        list_refresh();
    }
}

static void app_files_exit(void *scr)
{
}
