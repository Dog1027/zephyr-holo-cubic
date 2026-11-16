#pragma once

#include <stddef.h>
#include <stdint.h>

/*
 * Decode a baseline JPEG and write it straight to the display, centered.
 * This bypasses LVGL: call jpeg_begin() once LVGL has nothing left to redraw,
 * and invalidate the screen when switching back to LVGL drawing.
 */

// Make LVGL finish drawing now, after this the screen belongs to jpeg_show()
void jpeg_begin(void);

// Returns 0 on success, -ENOENT if the file does not exist, -EIO on decode errors
int jpeg_show(const char *path);

// Read the next len bytes into buf, or skip them when buf is NULL. Returns the bytes read,
// fewer than len only at the end of the data or on an error
typedef size_t (*jpeg_read_cb_t)(void *ctx, uint8_t *buf, size_t len);

// Same as jpeg_show() with the data coming from read (e.g. a socket), 0 or -EIO
int jpeg_show_stream(jpeg_read_cb_t read, void *ctx);
