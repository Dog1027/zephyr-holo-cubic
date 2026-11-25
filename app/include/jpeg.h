#pragma once

#include <stddef.h>
#include <stdint.h>
#include <zephyr/toolchain.h>

/*
 * Decode a baseline JPEG and write it straight to the display, centered.
 * This bypasses LVGL: call jpeg_begin() once LVGL has nothing left to redraw,
 * and invalidate the screen when switching back to LVGL drawing.
 *
 * The module owns no memory: the app that shows JPEGs keeps a jpeg_mem_t in its state
 * (self_t, the memory shared by the foreground apps) and hands it over with jpeg_begin().
 */

#define JPEG_MAX_WIDTH      240
#define JPEG_MCU_HEIGHT     16      // TJpgDec outputs one row of MCUs at a time, 16 high for 4:2:0
#define JPEG_WORKBUF_SIZE   12288   // the huffman tables of JD_FASTDECODE 2 take about 6 KB of it

// Working memory of the decoder, about 27 KB. Nothing in it needs to be initialized
typedef struct {
    uint8_t workbuf[JPEG_WORKBUF_SIZE];
    // Two row buffers taking turns: one is decoded into while the other goes to the display
    uint16_t row_buf[2][JPEG_MAX_WIDTH * JPEG_MCU_HEIGHT];
} __aligned(4) jpeg_mem_t;

// Make LVGL finish drawing now, after this the screen belongs to jpeg_show(), which works in mem
void jpeg_begin(jpeg_mem_t *mem);

// Stop using the memory given to jpeg_begin(). Call it in the app's exit()
void jpeg_end(void);

// Returns 0 on success, -ENOENT if the file does not exist, -EIO on decode errors
int jpeg_show(const char *path);

// Read the next len bytes into buf, or skip them when buf is NULL. Returns the bytes read,
// fewer than len only at the end of the data or on an error
typedef size_t (*jpeg_read_cb_t)(void *ctx, uint8_t *buf, size_t len);

// Same as jpeg_show() with the data coming from read (e.g. a socket), 0 or -EIO
int jpeg_show_stream(jpeg_read_cb_t read, void *ctx);
