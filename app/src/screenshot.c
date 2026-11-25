#include <zephyr/kernel.h>
#include <zephyr/drivers/uart.h>
#include <stdio.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/atomic.h>
#include <lvgl.h>
#include <display/lv_display_private.h>   // 原本的 flush_cb 沒有 getter
#include "screenshot.h"

/*
 * 截圖：shell 的 "shot" 讓 LVGL 重畫整個畫面，flush 時順便把每一列從序列埠印出來，
 * tools/screenshot.py 收回去組成 PNG。螢幕不能讀回來，也沒有記憶體放整張（240x240x2 = 112 KB），
 * 所以邊畫邊送。jpeg.h 直接畫到螢幕的相片和動畫不經過 LVGL，截不到。
 *
 * 輸出（一行一筆）：
 *   SHOT BEGIN <寬> <高> <LVGL color format>
 *   SHOT <y> <x> <寬> <base64>     一列的一段，資料是 RLE：[次數 1..255][像素 2 bytes]...
 *   SHOT END
 * 送一張約 3～10 秒（115200 baud，視畫面複雜度），期間畫面照常更新但會變慢。
 */

#define SHOT_MAX_WIDTH  240
#define SHOT_RLE_MAX    (SHOT_MAX_WIDTH * 3)
#define SHOT_B64_MAX    ((SHOT_RLE_MAX + 2) / 3 * 4 + 1)

// 直接寫 console UART：printk 走 log 的緩衝區，一次一百多 KB 會被丟掉大半
static const struct device *const uart = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));
static atomic_t requested;
static bool active;
static lv_display_flush_cb_t orig_flush;
// 放 static 不放 stack：flush 在 app thread 裡呼叫
static uint8_t rle[SHOT_RLE_MAX];
static char b64[SHOT_B64_MAX];

static void out(const char *s)
{
    while (*s != '\0') {
        uart_poll_out(uart, *s++);
    }
}

static size_t base64(char *dst, const uint8_t *src, size_t len)
{
    static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t n = 0;

    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = (uint32_t)src[i] << 16;
        if (i + 1 < len) {
            v |= (uint32_t)src[i + 1] << 8;
        }
        if (i + 2 < len) {
            v |= src[i + 2];
        }
        dst[n++] = table[(v >> 18) & 0x3F];
        dst[n++] = table[(v >> 12) & 0x3F];
        dst[n++] = i + 1 < len ? table[(v >> 6) & 0x3F] : '=';
        dst[n++] = i + 2 < len ? table[v & 0x3F] : '=';
    }
    dst[n] = '\0';
    return n;
}

static void row_send(int32_t x, int32_t y, int32_t w, const uint16_t *px)
{
    size_t n = 0;

    for (int32_t i = 0; i < w;) {
        uint16_t p = px[i];
        int32_t run = 1;
        while (i + run < w && run < 255 && px[i + run] == p) {
            run++;
        }
        rle[n++] = run;
        rle[n++] = p & 0xFF;
        rle[n++] = p >> 8;
        i += run;
    }
    char head[24];
    snprintf(head, sizeof(head), "\nSHOT %d %d %d ", (int)y, (int)x, (int)w);
    out(head);
    base64(b64, rle, n);
    out(b64);
    out("\n");
}

static void shot_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    if (active) {
        int32_t w = lv_area_get_width(area);
        uint32_t stride = lv_draw_buf_width_to_stride(w, lv_display_get_color_format(disp));
        for (int32_t y = area->y1; y <= area->y2 && w <= SHOT_MAX_WIDTH; y++) {
            row_send(area->x1, y, w, (const uint16_t *)(px_map + (y - area->y1) * stride));
        }
        if (lv_display_flush_is_last(disp)) {
            out("\nSHOT END\n");
            active = false;
        }
    }
    orig_flush(disp, area, px_map);
}

void screenshot_process(void)
{
    lv_display_t *disp = lv_display_get_default();

    if (active || !atomic_cas(&requested, 1, 0) || disp == NULL) {
        return;
    }
    // 只支援 16 bit，flush callback 第一次截圖時才換成自己的
    lv_color_format_t cf = lv_display_get_color_format(disp);
    if (lv_color_format_get_size(cf) != 2 || lv_display_get_horizontal_resolution(disp) > SHOT_MAX_WIDTH) {
        out("\nSHOT FAIL unsupported display\n");
        return;
    }
    if (orig_flush == NULL) {
        orig_flush = disp->flush_cb;
        lv_display_set_flush_cb(disp, shot_flush_cb);
    }
    char line[40];
    snprintf(line, sizeof(line), "\nSHOT BEGIN %d %d %d\n", (int)lv_display_get_horizontal_resolution(disp),
             (int)lv_display_get_vertical_resolution(disp), (int)cf);
    out(line);
    active = true;
    lv_obj_invalidate(lv_screen_active());
}

static int _shot(const struct shell *shell, size_t argc, char **argv)
{
    atomic_set(&requested, 1);
    return 0;
}

SHELL_CMD_REGISTER(shot, NULL, "Screenshot over the console (read with tools/screenshot.py)", _shot);
