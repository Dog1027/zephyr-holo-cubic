#include "fg_app.h"
#include "ui.h"
#include "ui_tabs.h"
#include "jpeg.h"
#include "led.h"
#include "file.h"
#include <zephyr/fs/fs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/*
 * SD 卡上的 JPEG，兩個分頁（ui_tabs.h）：
 *   PHOTOS：PHOTO_ROOT 底下的相片，全螢幕。上下換張，敲一下下一張，OK 開關幻燈片。
 *   ANIM：ANIM_ROOT 底下每個資料夾是一段動畫（000.jpg、001.jpg……，可選的 delay.txt 寫每格毫秒數）。
 *         上下選，OK 或敲一下播放 / 停止。
 * 都是 baseline JPEG（最大 240x240，tools/img2jpg.py 轉檔）。畫相片和動畫時繞過 LVGL 直接寫到螢幕
 * （jpeg.h），會蓋掉分頁籤：切到 PHOTOS 時分頁籤先顯示 GALLERY_TABS_MS 才開始畫，
 * 切換分頁就停下來，整個畫面交還給 LVGL 重畫。
 */

#define PHOTO_ROOT              FILE_ROOT "/photo"
#define PHOTO_SLIDE_MS          5000    // 幻燈片模式每張停留時間
#define PHOTO_BLINK_MS          150
#define PHOTO_BLINK_COLOR       THEME_CYAN
#define ANIM_ROOT               FILE_ROOT "/anim"
#define ANIM_MAX_ENTRIES        16
#define ANIM_DEFAULT_DELAY_MS   100
#define GALLERY_PATH_MAX        128
#define GALLERY_ACCENT_COLOR    THEME_CYAN
// 切到 PHOTOS（含剛打開 app）後分頁籤先顯示這麼久，看得出在哪一頁；也要等開 app 的淡入動畫畫完
#define GALLERY_TABS_MS         600

BUILD_ASSERT(GALLERY_TABS_MS > CONFIG_APP_THREAD_ANIM_DURATION_MS + 50,
             "photos must start after the app's fade-in");

enum { MODE_PHOTOS, MODE_ANIM, MODE_COUNT };

typedef struct {
    lv_obj_t *scr;
    lv_obj_t *pages[MODE_COUNT];    // 每個分頁一個容器，只顯示目前那個
    int mode;
    bool drawing;           // 畫面目前是 JPEG，不是 LVGL

    // PHOTOS
    int photo_count;
    int photo_index;
    int64_t photo_start_ms; // 這個時間之後才開始畫相片
    bool slideshow;
    int pending_step;       // 還沒處理的換張：下一張加 1，上一張減 1
    int64_t next_slide_ms;
    char *names;            // 檔名連續放在一起（lv_malloc），name_offsets 是每個的開頭
    size_t *name_offsets;

    // ANIM
    lv_obj_t *list;
    lv_obj_t *selected;     // 清單裡 highlight 的那一列（列不在 group 裡，自己記）
    char anim_path[GALLERY_PATH_MAX];
    bool playing;
    int frame;
    uint32_t delay_ms;
    int64_t next_frame_ms;

    jpeg_mem_t jpeg;        // 解碼用的工作區（約 27 KB），不用初始化
} self_t;

APP_FG_REGISTER(Gallery, ICON_GALLERY, app_gallery_init, app_gallery_run, app_gallery_exit, 7, self_t);

// 回到 LVGL：整個畫面標記重畫，蓋掉最後一張相片或一格動畫
static void drawing_stop(void)
{
    if (self->drawing) {
        self->drawing = false;
        lv_obj_invalidate(self->scr);
    }
}

static void drawing_start(void)
{
    if (!self->drawing) {
        jpeg_begin(&self->jpeg);
        self->drawing = true;
    }
}

// 透過稜鏡小字和灰字都看不清楚：兩行都是白色 18 px，說明盡量短
static void message_show(lv_obj_t *page, const char *text, const char *detail)
{
    lv_obj_t *label = ui_label(page, &lv_font_montserrat_18, THEME_TEXT);

    lv_label_set_text(label, text);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, UI_TABS_CENTER_Y - 24);

    label = ui_label(page, &lv_font_montserrat_18, THEME_TEXT);
    lv_label_set_text(label, detail);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, UI_TABS_CENTER_Y + 24);
}

/* ---------- PHOTOS ---------- */

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

// 從 index 開始往 step 方向找第一張能解碼的相片，全部失敗就回到 LVGL 說明原因
static void photo_show(int index, int step)
{
    char path[GALLERY_PATH_MAX];

    for (int tries = 0; tries < self->photo_count; tries++) {
        index = ((index % self->photo_count) + self->photo_count) % self->photo_count;
        int len = snprintf(path, sizeof(path), "%s/%s", PHOTO_ROOT, &self->names[self->name_offsets[index]]);
        int ret = (len < 0 || len >= (int)sizeof(path)) ? -ENAMETOOLONG : jpeg_show(path);
        if (ret == 0) {
            self->photo_index = index;
            LOG_INF("Photo %d/%d: %s", index + 1, self->photo_count, path);
            return;
        }
        LOG_ERR("Skip photo %d (%d)", index, ret);
        index += step;
    }
    // 一張都畫不出來（常見：沒轉檔的原圖，太大或是 progressive）：不再重試
    LOG_ERR("No photo could be shown");
    self->photo_count = 0;
    message_show(self->pages[MODE_PHOTOS], LV_SYMBOL_WARNING "  Cannot show\nthe photos", "Use JPEG, 240x240\nor smaller");
    drawing_stop();
}

static void photos_run(int64_t now)
{
    if (self->photo_count == 0 || now < self->photo_start_ms) {
        return;
    }
    if (!self->drawing) {
        // 從上次看到的那張繼續（加上等待時按的換張）
        drawing_start();
        int step = self->pending_step;
        self->pending_step = 0;
        photo_show(self->photo_index + step, step < 0 ? -1 : 1);
        self->next_slide_ms = k_uptime_get() + PHOTO_SLIDE_MS;
        return;
    }

    // 連按好幾次時一次跳過去，只解碼最後那一張
    int step = self->pending_step;
    self->pending_step = 0;
    if (step == 0 && self->slideshow && now >= self->next_slide_ms) {
        step = 1;
        led_flash(PHOTO_BLINK_COLOR, PHOTO_BLINK_MS);
    }
    if (step != 0) {
        photo_show(self->photo_index + step, step > 0 ? 1 : -1);
        self->next_slide_ms = k_uptime_get() + PHOTO_SLIDE_MS;
    }
}

/* ---------- ANIM ---------- */

static int frame_show(int index)
{
    char path[GALLERY_PATH_MAX + 16];

    snprintf(path, sizeof(path), "%s/%03d.jpg", self->anim_path, index);
    return jpeg_show(path);
}

static uint32_t delay_read(void)
{
    struct fs_file_t file;
    char path[GALLERY_PATH_MAX + 16];
    char buf[12] = {0};

    snprintf(path, sizeof(path), "%s/delay.txt", self->anim_path);
    fs_file_t_init(&file);
    if (!file_exists(path) || fs_open(&file, path, FS_O_READ) < 0) {
        return ANIM_DEFAULT_DELAY_MS;
    }
    fs_read(&file, buf, sizeof(buf) - 1);
    fs_close(&file);

    int ms = atoi(buf);
    return ms > 0 ? ms : ANIM_DEFAULT_DELAY_MS;
}

static void play_stop(void)
{
    self->playing = false;
    drawing_stop();
}

static void play_start(const char *name)
{
    int len = snprintf(self->anim_path, sizeof(self->anim_path), "%s/%s", ANIM_ROOT, name);
    if (len < 0 || len >= (int)sizeof(self->anim_path)) {
        LOG_WRN("Path too long: %s", name);
        return;
    }
    self->delay_ms = delay_read();
    self->frame = 0;
    self->next_frame_ms = k_uptime_get();
    LOG_INF("Play %s, %u ms per frame", self->anim_path, self->delay_ms);
    drawing_start();
    self->playing = true;
}

static void anim_run(int64_t now)
{
    if (!self->playing || now < self->next_frame_ms) {
        return;
    }

    int ret = frame_show(self->frame);
    if (ret == -ENOENT && self->frame > 0) {
        self->frame = 0;
        ret = frame_show(self->frame);
    }
    if (ret < 0) {
        LOG_ERR("Stop playing %s (%d)", self->anim_path, ret);
        play_stop();
        return;
    }

    self->frame++;
    self->next_frame_ms += self->delay_ms;
    // 解碼跟不上時不要累積延遲，直接從現在重新計時
    if (self->next_frame_ms < k_uptime_get()) {
        self->next_frame_ms = k_uptime_get();
    }
}

static void list_load(void)
{
    struct fs_dirent entry;
    struct fs_dir_t dir;
    int count = 0;

    fs_dir_t_init(&dir);
    if (!file_exists(ANIM_ROOT) || fs_opendir(&dir, ANIM_ROOT) < 0) {
        ui_list_message(self->list, "Put JPEG folders\nin /anim");
        return;
    }

    while (count < ANIM_MAX_ENTRIES && fs_readdir(&dir, &entry) == 0 && entry.name[0] != '\0') {
        if (entry.type == FS_DIR_ENTRY_DIR) {
            lv_obj_t *item = ui_list_item(self->list, LV_SYMBOL_PLAY, entry.name, NULL,
                                          lv_color_hex(GALLERY_ACCENT_COLOR));
            // 另存一份完整名稱，label 的文字太長時會被改成 "..."；OK 時從 user data 拿
            lv_obj_set_user_data(item, (void *)ui_obj_strdup(item, entry.name));
            count++;
        }
    }
    fs_closedir(&dir);

    if (count == 0) {
        ui_list_message(self->list, "No animations in /anim");
    }
    // 分頁籤佔著 next / prev，列不進 group，highlight 自己管
    ui_group_remove_children(self->list);
    self->selected = ui_list_step(self->list, NULL, 1);
}

/* ---------- 分頁 ---------- */

static void tab_show(int mode)
{
    play_stop();
    drawing_stop();
    self->mode = mode;
    for (int i = 0; i < MODE_COUNT; i++) {
        lv_obj_set_hidden(self->pages[i], i != mode);
    }
    if (mode == MODE_PHOTOS) {
        self->photo_start_ms = k_uptime_get() + GALLERY_TABS_MS;
    }
}

// PHOTOS：敲一下下一張，OK 開關幻燈片。ANIM：播放選取的動畫 / 停止
static void tab_action(int mode, bool tap)
{
    if (mode == MODE_PHOTOS) {
        if (tap) {
            self->pending_step++;
            return;
        }
        self->slideshow = !self->slideshow;
        self->next_slide_ms = k_uptime_get() + PHOTO_SLIDE_MS;
        LOG_INF("Slideshow %s", self->slideshow ? "on" : "off");
        led_flash(PHOTO_BLINK_COLOR, PHOTO_BLINK_MS);
        return;
    }
    if (self->playing) {
        play_stop();
    }
    else if (self->selected != NULL && lv_obj_get_user_data(self->selected) != NULL) {
        play_start(lv_obj_get_user_data(self->selected));
    }
}

// PHOTOS：上一張 / 下一張。ANIM：在清單裡移動
static void tab_scroll(int mode, int dir)
{
    if (mode == MODE_PHOTOS) {
        self->pending_step += dir;
    }
    else if (!self->playing) {
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

static void app_gallery_init(void *scr)
{
    ui_screen_init(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(THEME_BLACK), 0);
    self->scr = scr;

    self->pages[MODE_PHOTOS] = page_create(scr);
    self->photo_count = photos_load();
    LOG_INF("%d photos in %s", self->photo_count, PHOTO_ROOT);
    if (self->photo_count == 0) {
        message_show(self->pages[MODE_PHOTOS], "No photos", "Put JPEGs in /photo");
    }

    self->pages[MODE_ANIM] = page_create(scr);
    self->list = ui_list_create(self->pages[MODE_ANIM]);
    lv_obj_set_height(self->list, UI_SCREEN_SIZE - UI_TABS_BOTTOM - 4);
    list_load();

    // 建立時就會呼叫 tab_show，內容要先建好
    static const char *const names[] = {"PHOTOS", "ANIM"};
    static const ui_tabs_cfg_t tabs = {
        .names = names, .count = ARRAY_SIZE(names), .accent = GALLERY_ACCENT_COLOR,
        .pref_key = "gallery/mode", .show = tab_show, .action = tab_action, .scroll = tab_scroll,
    };
    ui_tabs_create(scr, &tabs);
}

static void app_gallery_run(void *scr)
{
    int64_t now = k_uptime_get();

    if (self->mode == MODE_PHOTOS) {
        photos_run(now);
    }
    else {
        anim_run(now);
    }
}

static void app_gallery_exit(void *scr)
{
    jpeg_end();
    led_restore();
    photos_free();
}
