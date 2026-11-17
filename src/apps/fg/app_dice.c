#include "fg_app.h"
#include "ui.h"
#include "led.h"
#include "nav.h"
#include "file.h"
#include <zephyr/fs/fs.h>
#include <zephyr/random/random.h>
#include <string.h>

// 抽籤清單：每行一個選項，只能用英數字 (字型沒有中文)
#define LOTS_FILE           FILE_ROOT "/lots.txt"
#define LOTS_MAX            32
#define LOTS_LEN            40
#define DICE_SIZE           110
#define DICE_PIP_SIZE       18
#define DICE_PIP_OFFSET     30
#define ROLL_MS             700   // 停止搖晃後再滾這麼久才停
#define ROLL_STEP_MS        60
#define LED_HOLD_MS         800
#define DICE_ACCENT_COLOR   THEME_RED

enum { MODE_DICE, MODE_LOTS };

// 3x3 格子上的 7 個點：左上、右上、左中、中、右中、左下、右下
static const int8_t pip_pos[7][2] = {
    {-1, -1}, {1, -1}, {-1, 0}, {0, 0}, {1, 0}, {-1, 1}, {1, 1},
};
static const uint8_t pip_mask[7] = {
    0,
    BIT(3),
    BIT(0) | BIT(6),
    BIT(0) | BIT(3) | BIT(6),
    BIT(0) | BIT(1) | BIT(5) | BIT(6),
    BIT(0) | BIT(1) | BIT(3) | BIT(5) | BIT(6),
    BIT(0) | BIT(1) | BIT(2) | BIT(4) | BIT(5) | BIT(6),
};

typedef struct {
    lv_obj_t *dice;
    lv_obj_t *pips[7];
    lv_obj_t *lot_label;
    char lots[LOTS_MAX][LOTS_LEN + 1];
    int lots_count;
    int mode;
    bool rolling;
    int64_t roll_end_ms;
    int64_t next_step_ms;
} self_t;

APP_FG_REGISTER(Dice, ICON_DICE, app_dice_init, app_dice_run, app_dice_exit, 10, self_t);

static void lots_load(void)
{
    // 只在讀檔時用，放 stack 不放 self（app_thread 的 stack 有餘裕）
    char buf[LOTS_MAX * (LOTS_LEN + 2)];
    struct fs_file_t file;

    self->lots_count = 0;
    fs_file_t_init(&file);
    if (!file_exists(LOTS_FILE) || fs_open(&file, LOTS_FILE, FS_O_READ) < 0) {
        return;
    }
    ssize_t len = fs_read(&file, buf, sizeof(buf) - 1);
    fs_close(&file);
    if (len <= 0) {
        return;
    }
    buf[len] = '\0';

    for (char *line = strtok(buf, "\r\n"); line != NULL && self->lots_count < LOTS_MAX; line = strtok(NULL, "\r\n")) {
        if (line[0] != '\0') {
            strncpy(self->lots[self->lots_count], line, LOTS_LEN);
            self->lots[self->lots_count][LOTS_LEN] = '\0';
            self->lots_count++;
        }
    }
}

static void dice_show(int value)
{
    for (int i = 0; i < 7; i++) {
        if (pip_mask[value] & BIT(i)) {
            lv_obj_set_hidden(self->pips[i], false);
        }
        else {
            lv_obj_set_hidden(self->pips[i], true);
        }
    }
}

static void roll_step(void)
{
    if (self->mode == MODE_DICE) {
        dice_show(1 + sys_rand32_get() % 6);
    }
    else if (self->lots_count > 0) {
        lv_label_set_text(self->lot_label, self->lots[sys_rand32_get() % self->lots_count]);
    }
}

static void mode_set(int new_mode)
{
    self->mode = new_mode;
    if (self->rolling) {
        // 擲到一半切換分頁，LED 不能停在滾動中的隨機顏色
        self->rolling = false;
        led_restore();
    }
    if (self->mode == MODE_DICE) {
        lv_obj_set_hidden(self->dice, false);
        lv_obj_set_hidden(self->lot_label, true);
    }
    else {
        lv_obj_set_hidden(self->dice, true);
        lv_obj_set_hidden(self->lot_label, false);
        lv_label_set_text(self->lot_label, self->lots_count > 0 ? "?" : "Put one item per line\nin /lots.txt");
    }
}

static void tab_focused_cb(lv_event_t *e)
{
    mode_set((int)(intptr_t)lv_event_get_user_data(e));
}

// 搖晃會連續觸發敲擊，每次都重新計時，停手後才停下來
static void tab_clicked_cb(lv_event_t *e)
{
    if (self->mode == MODE_LOTS && self->lots_count == 0) {
        return;
    }
    int64_t now = k_uptime_get();
    self->rolling = true;
    self->roll_end_ms = now + ROLL_MS;
    self->next_step_ms = now;
}

// 敲擊 / 搖晃是擲骰，不是切換分頁；next / prev 切分頁，ok 擲
static bool nav_cb(nav_cmd_t cmd)
{
    if (cmd != NAV_TAP) {
        return false;
    }
    tab_clicked_cb(NULL);
    return true;
}

static void app_dice_init(void *scr)
{
    ui_screen_init(scr);
    lots_load();

    self->dice = lv_obj_create(scr);
    lv_obj_remove_style_all(self->dice);
    lv_obj_set_size(self->dice, DICE_SIZE, DICE_SIZE);
    lv_obj_set_style_radius(self->dice, 22, 0);
    lv_obj_set_style_bg_color(self->dice, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(self->dice, LV_OPA_COVER, 0);
    lv_obj_align(self->dice, LV_ALIGN_CENTER, 0, 6);
    lv_obj_set_scrollable(self->dice, false);
    lv_obj_set_clickable(self->dice, false);

    for (int i = 0; i < 7; i++) {
        self->pips[i] = lv_obj_create(self->dice);
        lv_obj_remove_style_all(self->pips[i]);
        lv_obj_set_size(self->pips[i], DICE_PIP_SIZE, DICE_PIP_SIZE);
        lv_obj_set_style_radius(self->pips[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(self->pips[i], lv_color_hex(THEME_BG), 0);
        lv_obj_set_style_bg_opa(self->pips[i], LV_OPA_COVER, 0);
        lv_obj_align(self->pips[i], LV_ALIGN_CENTER, pip_pos[i][0] * DICE_PIP_OFFSET, pip_pos[i][1] * DICE_PIP_OFFSET);
        lv_obj_set_clickable(self->pips[i], false);
    }
    dice_show(6);

    self->lot_label = lv_label_create(scr);
    lv_obj_set_width(self->lot_label, 200);
    lv_label_set_long_mode(self->lot_label, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(self->lot_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(self->lot_label, lv_color_hex(THEME_TEXT), 0);
    lv_obj_align(self->lot_label, LV_ALIGN_CENTER, 0, 6);

    lv_obj_t *hint = lv_label_create(scr);
    lv_label_set_text(hint, "Tap or shake");
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(THEME_TEXT_DIM), 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -12);

    // 第一個 tab 會馬上被 focus，觸發 mode_set，所以內容要先建好
    static const char *const names[] = {"DICE", "LOTS"};
    ui_tabs(scr, names, ARRAY_SIZE(names), lv_color_hex(DICE_ACCENT_COLOR), tab_focused_cb, tab_clicked_cb);
    nav_set_handler(nav_cb);
}

static void app_dice_run(void *scr)
{
    int64_t now = k_uptime_get();

    if (!self->rolling || now < self->next_step_ms) {
        return;
    }

    roll_step();
    if (now < self->roll_end_ms) {
        led_set_hex(sys_rand32_get() & 0xffffff);
        self->next_step_ms = now + ROLL_STEP_MS;
        return;
    }

    self->rolling = false;
    led_flash(DICE_ACCENT_COLOR, LED_HOLD_MS);
}

static void app_dice_exit(void *scr)
{
    led_restore();
}
