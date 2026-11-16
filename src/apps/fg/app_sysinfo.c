#include "fg_app.h"
#include "file.h"
#include "ui.h"
#include "input.h"
#include <zephyr/kernel.h>
#include <zephyr/fs/fs.h>
#include <zephyr/sys/sys_heap.h>
#include <zephyr/version.h>
#include <lvgl_mem.h>
#include <string.h>
#include <stdio.h>

#define SYSINFO_ACCENT_COLOR    0x9b6cf2
#define SYSINFO_REFRESH_MS      500
#define SYSINFO_SD_REFRESH_MS   5000
#define SYSINFO_SD_PATH         FILE_ROOT
#define SYSINFO_MAX_THREADS     12  // 每列都佔 LVGL 記憶體，總列數控制在 ~20

extern struct k_heap _system_heap;

enum {
    ROW_UPTIME,
    ROW_SYS_HEAP,
    ROW_LVGL_HEAP,
    ROW_SD,
    ROW_ROLL,
    ROW_PITCH,
    ROW_FIXED_COUNT,
};

// app 開著時的狀態，放在共用的 app_state_mem（fg_app.h），每次打開前清成 0
typedef struct {
    lv_obj_t *rows[ROW_FIXED_COUNT];          // 每一列右邊的值
    lv_obj_t *thread_rows[SYSINFO_MAX_THREADS];
    k_tid_t threads[SYSINFO_MAX_THREADS];
    int thread_count;
    int64_t last_refresh;
    int64_t last_sd_refresh;
} self_t;

APP_FG_REGISTER(SysInfo, LV_SYMBOL_LIST, app_sysinfo_init, app_sysinfo_run, app_sysinfo_exit, 13, self_t);

static lv_obj_t *row_create(lv_obj_t *list, const char *icon, const char *name)
{
    return ui_list_row(list, icon, name, lv_color_hex(SYSINFO_ACCENT_COLOR));
}

// 角度轉成一位小數的字串，不用 %f (picolibc 可能沒有浮點 printf)
static void row_set_angle(lv_obj_t *row, float deg)
{
    int tenths = (int)(deg * 10.0f + (deg < 0 ? -0.5f : 0.5f));
    const char *sign = tenths < 0 ? "-" : "";

    if (tenths < 0) {
        tenths = -tenths;
    }
    ui_label_set_fmt(row, "%s%d.%d deg", sign, tenths / 10, tenths % 10);
}

static void thread_collect_cb(const struct k_thread *thread, void *user_data)
{
    if (self->thread_count < SYSINFO_MAX_THREADS) {
        self->threads[self->thread_count++] = (k_tid_t)thread;
    }
}

static void update_uptime(void)
{
    uint32_t sec = (uint32_t)(k_uptime_get() / 1000);
    uint32_t days = sec / 86400;

    sec %= 86400;
    if (days > 0) {
        ui_label_set_fmt(self->rows[ROW_UPTIME], "%ud %02u:%02u:%02u", (unsigned int)days, (unsigned int)(sec / 3600),
                (unsigned int)(sec / 60 % 60), (unsigned int)(sec % 60));
    }
    else {
        ui_label_set_fmt(self->rows[ROW_UPTIME], "%u:%02u:%02u", (unsigned int)(sec / 3600), (unsigned int)(sec / 60 % 60),
                (unsigned int)(sec % 60));
    }
}

static void update_heap(lv_obj_t *row, const struct sys_memory_stats *stats)
{
    size_t total = stats->allocated_bytes + stats->free_bytes;

    ui_label_set_fmt(row, "%u / %u KB", (unsigned int)((stats->allocated_bytes + 1023) / 1024),
            (unsigned int)(total / 1024));
}

static void update_sd(void)
{
    struct fs_statvfs st;

    if (fs_statvfs(SYSINFO_SD_PATH, &st) < 0 || st.f_blocks == 0) {
        ui_label_set_fmt(self->rows[ROW_SD], "No card");
        return;
    }

    uint64_t total = (uint64_t)st.f_frsize * st.f_blocks;
    uint64_t used = (uint64_t)st.f_frsize * (st.f_blocks - st.f_bfree);
    ui_label_set_fmt(self->rows[ROW_SD], "%u / %u MB", (unsigned int)(used >> 20), (unsigned int)(total >> 20));
}

static void update_threads(void)
{
    for (int i = 0; i < self->thread_count; i++) {
        size_t unused;

        // 執行緒結束後 stack 可能已無效，查詢失敗就顯示 "-"
        if (k_thread_stack_space_get(self->threads[i], &unused) != 0) {
            ui_label_set_fmt(self->thread_rows[i], "-");
            continue;
        }

        size_t size = self->threads[i]->stack_info.size;
        ui_label_set_fmt(self->thread_rows[i], "%u / %u", (unsigned int)(size - unused), (unsigned int)size);
    }
}

static void refresh(bool sd)
{
    struct sys_memory_stats stats;
    input_angle_t angle;

    update_uptime();

    sys_heap_runtime_stats_get(&_system_heap.heap, &stats);
    update_heap(self->rows[ROW_SYS_HEAP], &stats);

    lvgl_heap_stats(&stats);
    update_heap(self->rows[ROW_LVGL_HEAP], &stats);

    if (sd) {
        update_sd();
    }

    input_get(&angle);
    row_set_angle(self->rows[ROW_ROLL], angle.roll);
    row_set_angle(self->rows[ROW_PITCH], angle.pitch);

    update_threads();
}

static void app_sysinfo_init(void *scr)
{
    ui_screen_init(scr);
    ui_title(scr, "System");
    lv_obj_t *list = ui_list_create(scr);

    self->rows[ROW_UPTIME] = row_create(list, LV_SYMBOL_REFRESH, "Uptime");
    self->rows[ROW_SYS_HEAP] = row_create(list, LV_SYMBOL_DRIVE, "Sys heap");
    self->rows[ROW_LVGL_HEAP] = row_create(list, LV_SYMBOL_IMAGE, "LVGL heap");
    self->rows[ROW_SD] = row_create(list, LV_SYMBOL_SD_CARD, "SD card");
    self->rows[ROW_ROLL] = row_create(list, LV_SYMBOL_GPS, "Roll");
    self->rows[ROW_PITCH] = row_create(list, LV_SYMBOL_GPS, "Pitch");

    // 先收集執行緒再建立列，不能在 foreach callback 裡呼叫 LVGL
    k_thread_foreach_unlocked(thread_collect_cb, NULL);
    for (int i = 0; i < self->thread_count; i++) {
        const char *name = k_thread_name_get(self->threads[i]);
        char fallback[16];

        if (name == NULL || name[0] == '\0') {
            snprintf(fallback, sizeof(fallback), "%p", (void *)self->threads[i]);
            name = fallback;
        }
        self->thread_rows[i] = row_create(list, LV_SYMBOL_SETTINGS, name);
    }

    ui_list_item(list, LV_SYMBOL_HOME, "Board", CONFIG_BOARD, lv_color_hex(SYSINFO_ACCENT_COLOR));
    ui_list_item(list, LV_SYMBOL_EYE_OPEN, "Zephyr", KERNEL_VERSION_STRING, lv_color_hex(SYSINFO_ACCENT_COLOR));

    refresh(true);
    self->last_refresh = k_uptime_get();
    self->last_sd_refresh = self->last_refresh;
}

static void app_sysinfo_run(void *scr)
{
    int64_t now = k_uptime_get();

    if (now - self->last_refresh < SYSINFO_REFRESH_MS) {
        return;
    }
    self->last_refresh = now;

    bool sd = now - self->last_sd_refresh >= SYSINFO_SD_REFRESH_MS;
    if (sd) {
        self->last_sd_refresh = now;
    }
    refresh(sd);
}

static void app_sysinfo_exit(void *scr)
{
    // LVGL 物件由框架刪除
}
