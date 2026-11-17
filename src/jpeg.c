#include <zephyr/kernel.h>
#include <zephyr/drivers/display.h>
#include <zephyr/fs/fs.h>
#include <zephyr/logging/log.h>
#include <lvgl.h>
#include <string.h>
#include "lib/tjpgd/tjpgd.h"
#include "jpeg.h"
#include "file.h"

LOG_MODULE_REGISTER(jpeg, CONFIG_CUBE_LOG_LEVEL);

#define JPEG_WRITER_STACK 1024  // 實測最多 464
#define JPEG_WRITER_PRIO  6      // 比 app_thread 高，buffer 一交出去就開始傳

typedef struct {
    jpeg_read_cb_t read;
    void *ctx;
    int x0;
    int y0;
} jpeg_dec_t;

typedef struct {
    uint8_t buf;
    uint16_t x;
    uint16_t y;
    uint16_t w;
    uint16_t h;
} jpeg_write_t;

static const struct device *display_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));

// 工作區由 app 提供（jpeg_begin），放在前景 app 共用的 state 裡；沒有 app 在顯示 JPEG 時是 NULL
static jpeg_mem_t *mem;
static K_SEM_DEFINE(buf0_free, 1, 1);
static K_SEM_DEFINE(buf1_free, 1, 1);
static struct k_sem *const buf_free[2] = {&buf0_free, &buf1_free};
K_MSGQ_DEFINE(jpeg_write_q, sizeof(jpeg_write_t), 2, 4);

static jpeg_dec_t dec;
static int cur_buf;
static bool cur_taken;
// 上一張圖的大小，尺寸改變時要先把整個畫面清成黑色，避免留下舊圖的邊
static uint16_t last_w;
static uint16_t last_h;

#ifdef CONFIG_LV_Z_FLUSH_THREAD
#include <display/lv_display_private.h>
// 等 Zephyr LVGL glue 的 flush thread 把最後一塊送完。
// 原本直接呼叫 glue 的 lvgl_wait_cb()，新版改成 static，只能透過 flush_wait_cb 呼叫
static void lvgl_flush_wait(lv_display_t *disp)
{
    if (disp->flushing && disp->flush_wait_cb) {
        disp->flush_wait_cb(disp);
        disp->flushing = 0;
    }
    disp->flushing_last = 0;
}
#endif

static void writer_entry(void *arg1, void *arg2, void *arg3)
{
    jpeg_write_t w;

    while (1) {
        k_msgq_get(&jpeg_write_q, &w, K_FOREVER);
        struct display_buffer_descriptor desc = {
            .buf_size = w.w * w.h * 2,
            .width = w.w,
            .height = w.h,
            .pitch = w.w,
        };
        display_write(display_dev, w.x, w.y, &desc, mem->row_buf[w.buf]);
        k_sem_give(buf_free[w.buf]);
    }
}

K_THREAD_DEFINE(jpeg_writer, JPEG_WRITER_STACK, writer_entry, NULL, NULL, NULL, JPEG_WRITER_PRIO, 0, 0);

// 等兩個 buffer 都送完，之後畫面才完全是這張圖
static void writer_flush(void)
{
    if (cur_taken) {
        k_sem_give(buf_free[cur_buf]);
        cur_taken = false;
    }
    for (int i = 0; i < 2; i++) {
        k_sem_take(buf_free[i], K_FOREVER);
    }
    for (int i = 0; i < 2; i++) {
        k_sem_give(buf_free[i]);
    }
}

static size_t dec_input(JDEC *jd, uint8_t *buf, size_t len)
{
    jpeg_dec_t *d = jd->device;
    return d->read(d->ctx, buf, len);
}

static size_t file_read(void *ctx, uint8_t *buf, size_t len)
{
    struct fs_file_t *file = ctx;

    if (buf == NULL) {
        return fs_seek(file, len, FS_SEEK_CUR) == 0 ? len : 0;
    }
    ssize_t ret = fs_read(file, buf, len);
    return ret < 0 ? 0 : (size_t)ret;
}

// TJpgDec 依序輸出每個 MCU (RGB565)，湊滿一整列 MCU 後交給 writer thread
static int dec_output(JDEC *jd, void *bitmap, JRECT *rect)
{
    jpeg_dec_t *d = jd->device;
    const uint16_t *src = bitmap;
    uint16_t w = rect->right - rect->left + 1;
    uint16_t h = rect->bottom - rect->top + 1;

    if (!cur_taken) {
        k_sem_take(buf_free[cur_buf], K_FOREVER);
        cur_taken = true;
    }

    for (uint16_t y = 0; y < h; y++) {
        uint16_t *dst = &mem->row_buf[cur_buf][y * jd->width + rect->left];
        for (uint16_t x = 0; x < w; x++) {
            // ST7789V 吃 big-endian RGB565 (跟 LV_COLOR_16_SWAP 一樣)
            *dst++ = __builtin_bswap16(*src++);
        }
    }

    if (rect->right == jd->width - 1) {
        jpeg_write_t req = {
            .buf = cur_buf,
            .x = d->x0,
            .y = d->y0 + rect->top,
            .w = jd->width,
            .h = h,
        };
        k_msgq_put(&jpeg_write_q, &req, K_FOREVER);
        cur_taken = false;
        cur_buf ^= 1;
    }
    return 1;
}

static void screen_clear(uint16_t w, uint16_t h)
{
    struct display_buffer_descriptor desc = {
        .width = w,
        .pitch = w,
    };

    memset(mem->row_buf[0], 0, sizeof(mem->row_buf[0]));
    for (uint16_t y = 0; y < h; y += JPEG_MCU_HEIGHT) {
        desc.height = MIN(JPEG_MCU_HEIGHT, h - y);
        desc.buf_size = w * desc.height * 2;
        display_write(display_dev, 0, y, &desc, mem->row_buf[0]);
    }
}

void jpeg_begin(jpeg_mem_t *app_mem)
{
    mem = app_mem;
    // LVGL 畫完之後畫面是全黑的背景，可以當成「全螢幕大小的圖」
    lv_refr_now(NULL);
#ifdef CONFIG_LV_Z_FLUSH_THREAD
    lvgl_flush_wait(lv_display_get_default());
#endif
    last_w = 0;
    last_h = 0;
}

void jpeg_end(void)
{
    // jpeg_show() 回來之前就等 writer thread 送完了，這裡不會有人還在用
    mem = NULL;
}

int jpeg_show_stream(jpeg_read_cb_t read, void *ctx)
{
    struct display_capabilities caps;
    JDEC jd;
    JRESULT rc;

    dec.read = read;
    dec.ctx = ctx;
    display_get_capabilities(display_dev, &caps);
    if (mem == NULL) {
        return -EINVAL;
    }
    rc = jd_prepare(&jd, dec_input, mem->workbuf, sizeof(mem->workbuf), &dec);
    if (rc != JDR_OK) {
        // 最常見的原因是 progressive JPEG，TJpgDec 只支援 baseline
        LOG_ERR("Failed to decode (%d)", rc);
        return -EIO;
    }
    if (jd.width > MIN(caps.x_resolution, JPEG_MAX_WIDTH) || jd.height > caps.y_resolution) {
        LOG_ERR("Image is %ux%u, larger than the screen", jd.width, jd.height);
        return -EIO;
    }

    if (last_w != 0 && (jd.width != last_w || jd.height != last_h)) {
        writer_flush();
        screen_clear(caps.x_resolution, caps.y_resolution);
    }
    last_w = jd.width;
    last_h = jd.height;

    dec.x0 = (caps.x_resolution - jd.width) / 2;
    dec.y0 = (caps.y_resolution - jd.height) / 2;
    rc = jd_decomp(&jd, dec_output, 0);
    writer_flush();
    return rc == JDR_OK ? 0 : -EIO;
}

int jpeg_show(const char *path)
{
    struct fs_file_t file;
    int ret;

    // Anim 用「下一格不存在」判斷播完一輪，這是正常情況，不要讓 fs_open 印錯誤
    if (!file_exists(path)) {
        return -ENOENT;
    }
    fs_file_t_init(&file);
    ret = fs_open(&file, path, FS_O_READ);
    if (ret < 0) {
        return ret;
    }
    ret = jpeg_show_stream(file_read, &file);
    if (ret < 0) {
        LOG_ERR("Cannot show %s", path);
    }
    fs_close(&file);
    return ret;
}
