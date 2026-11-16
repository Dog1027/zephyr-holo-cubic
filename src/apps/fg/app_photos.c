#include "fg_app.h"
#include "ui.h"
#include "jpeg.h"
#include "led.h"
#include "nav.h"
#include "file.h"
#include <zephyr/fs/fs.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

// 相簿：顯示 PHOTO_ROOT 底下的 baseline JPEG（最大 240x240），用 tools/img2jpg.py 轉檔
#define PHOTO_ROOT            "/0:/photo"
#define PHOTO_PATH_MAX        128
#define PHOTO_SLIDE_MS        5000    // 幻燈片模式每張停留時間
#define PHOTO_START_DELAY_MS  50      // 淡入動畫結束後再多等一下才開始直接畫
#define PHOTO_BLINK_MS        150
#define PHOTO_BLINK_COLOR     0x2ec4d6

// app 開著時的狀態，放在共用的 app_state_mem（fg_app.h），每次打開前清成 0
typedef struct {
    int photo_count;
    int photo_index;
    bool started;
    bool slideshow;
    bool pending_toggle;
    int pending_step;  // 還沒處理的換張：next / tap 加 1，prev 減 1
    int64_t start_ms;
    int64_t next_slide_ms;
    int64_t led_off_ms;
    char *names;
    size_t *name_offsets;
} self_t;

APP_FG_REGISTER(Photos, LV_SYMBOL_IMAGE, app_photos_init, app_photos_run, app_photos_exit, 8, self_t);


static void photos_free(void)
{
    lv_free(self->names);
    lv_free(self->name_offsets);
    self->names = NULL;
    self->name_offsets = NULL;
}

static bool is_jpeg(const struct fs_dirent *entry)
{
    if (entry->type != FS_DIR_ENTRY_FILE) {
        return false;
    }
    const char *dot = strrchr(entry->name, '.');
    return dot != NULL && (strcasecmp(dot, ".jpg") == 0 || strcasecmp(dot, ".jpeg") == 0);
}

// 開啟時讀一次資料夾，把檔名連續存在一塊剛好大小的記憶體裡，換張時不用再讀 SD 卡
static int photos_load(void)
{
    struct fs_dirent entry;
    struct fs_dir_t dir;
    size_t total = 0;
    int count = 0;

    // 第一次掃描：算張數和檔名總長度
    fs_dir_t_init(&dir);
    if (!file_exists(PHOTO_ROOT) || fs_opendir(&dir, PHOTO_ROOT) < 0) {
        return 0;
    }
    while (fs_readdir(&dir, &entry) == 0 && entry.name[0] != '\0') {
        if (is_jpeg(&entry)) {
            total += strlen(entry.name) + 1;
            count++;
        }
    }
    fs_closedir(&dir);
    if (count == 0) {
        return 0;
    }

    self->names = lv_malloc(total);
    self->name_offsets = lv_malloc(count * sizeof(*self->name_offsets));
    if (self->names == NULL || self->name_offsets == NULL) {
        LOG_ERR("Not enough memory for %d photo names", count);
        photos_free();
        return 0;
    }

    // 第二次掃描：存檔名，SD 卡內容在兩次之間變了也不會超出配置的大小
    size_t used = 0;
    int n = 0;
    fs_dir_t_init(&dir);
    if (fs_opendir(&dir, PHOTO_ROOT) == 0) {
        while (n < count && fs_readdir(&dir, &entry) == 0 && entry.name[0] != '\0') {
            size_t len = strlen(entry.name) + 1;
            if (!is_jpeg(&entry) || used + len > total) {
                continue;
            }
            memcpy(&self->names[used], entry.name, len);
            self->name_offsets[n++] = used;
            used += len;
        }
        fs_closedir(&dir);
    }
    if (n == 0) {
        photos_free();
    }
    return n;
}

static int photo_find(int index, char *path, size_t size)
{
    int len = snprintf(path, size, "%s/%s", PHOTO_ROOT, &self->names[self->name_offsets[index]]);
    return (len < 0 || len >= (int)size) ? -ENAMETOOLONG : 0;
}

// 從 index 開始往 step 方向找第一張能解碼的照片，全部失敗就停在原處
static void photo_show(int index, int step)
{
    char path[PHOTO_PATH_MAX];

    for (int tries = 0; tries < self->photo_count; tries++) {
        index = ((index % self->photo_count) + self->photo_count) % self->photo_count;
        int ret = photo_find(index, path, sizeof(path));
        if (ret == 0) {
            ret = jpeg_show(path);
        }
        if (ret == 0) {
            self->photo_index = index;
            LOG_INF("Photo %d/%d: %s", index + 1, self->photo_count, path);
            return;
        }
        LOG_ERR("Skip photo %d (%d)", index, ret);
        index += step;
    }
    LOG_ERR("No photo could be shown");
}

static void blink(void)
{
    led_set_hex(PHOTO_BLINK_COLOR);
    self->led_off_ms = k_uptime_get() + PHOTO_BLINK_MS;
}

// next / tap 下一張，prev 上一張，ok 開關幻燈片；只記下來，交給 run 畫
static bool nav_cb(nav_cmd_t cmd)
{
    switch (cmd) {
    case NAV_NEXT:
    case NAV_TAP:
        self->pending_step++;
        return true;
    case NAV_PREV:
        self->pending_step--;
        return true;
    case NAV_OK:
        self->pending_toggle = true;
        return true;
    default:
        return false;
    }
}

static void app_photos_init(void *scr)
{
    ui_screen_init(scr);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);

    self->started = false;
    self->slideshow = false;
    self->pending_toggle = false;
    self->pending_step = 0;
    self->photo_index = 0;
    self->led_off_ms = 0;
    self->start_ms = k_uptime_get();

    self->photo_count = photos_load();
    LOG_INF("%d photos in %s", self->photo_count, PHOTO_ROOT);

    if (self->photo_count == 0) {
        ui_title(scr, "Photos");
        lv_obj_t *label = lv_label_create(scr);
        lv_label_set_text(label, "Put JPEG files in\n/photo on the SD card");
        lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(label, lv_color_hex(UI_DIM_COLOR), 0);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(label);
        return;
    }

    nav_set_handler(nav_cb);
}

static void app_photos_run(void *scr)
{
    int64_t now = k_uptime_get();

    if (self->led_off_ms != 0 && now >= self->led_off_ms) {
        self->led_off_ms = 0;
        led_restore();
    }

    if (self->photo_count == 0) {
        return;
    }

    // 淡入動畫期間 LVGL 每格都會重畫整個畫面，等它結束才開始直接畫
    if (!self->started) {
        if (now < self->start_ms + CONFIG_APP_THREAD_ANIM_DURATION_MS + PHOTO_START_DELAY_MS) {
            self->pending_toggle = false;
            self->pending_step = 0;
            return;
        }
        jpeg_begin();
        self->started = true;
        photo_show(0, 1);
        self->next_slide_ms = k_uptime_get() + PHOTO_SLIDE_MS;
        return;
    }

    if (self->pending_toggle) {
        self->pending_toggle = false;
        self->slideshow = !self->slideshow;
        self->next_slide_ms = now + PHOTO_SLIDE_MS;
        LOG_INF("Slideshow %s", self->slideshow ? "on" : "off");
        blink();
    }

    // 連按好幾次 next 時一次跳過去，只解碼最後那一張
    int step = self->pending_step;
    self->pending_step = 0;

    if (step == 0 && self->slideshow && now >= self->next_slide_ms) {
        step = 1;
        blink();
    }

    if (step != 0) {
        photo_show(self->photo_index + step, step > 0 ? 1 : -1);
        self->next_slide_ms = k_uptime_get() + PHOTO_SLIDE_MS;
    }
}

static void app_photos_exit(void *scr)
{
    self->started = false;
    self->slideshow = false;
    self->pending_toggle = false;
    self->pending_step = 0;
    self->photo_count = 0;
    self->led_off_ms = 0;
    led_restore();
    photos_free();
}
