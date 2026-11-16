#include <zephyr/kernel.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/logging/log.h>
#include "led.h"
#include "prefs.h"

LOG_MODULE_REGISTER(led, CONFIG_CUBE_LOG_LEVEL);

#define LED_WRITE_TRIES 4

static const struct device *led_dev = DEVICE_DT_GET_OR_NULL(DT_NODELABEL(rgb_led));
// ws2812 掛在 I2S 下面
static const struct device *i2s_dev = DEVICE_DT_GET_OR_NULL(DT_PARENT(DT_NODELABEL(rgb_led)));
// app thread、shell 和 Timer 的 work queue 都會寫 LED
static K_MUTEX_DEFINE(led_mutex);
static uint32_t base_rgb;

int led_set_raw(uint8_t r, uint8_t g, uint8_t b)
{
    if (led_dev == NULL || !device_is_ready(led_dev)) {
        return -ENODEV;
    }

    struct led_rgb pixel = { .r = r, .g = g, .b = b };
    int ret = 0;

    k_mutex_lock(&led_mutex, K_FOREVER);
    for (int i = 0; i < LED_WRITE_TRIES; i++) {
        ret = led_strip_update_rgb(led_dev, &pixel, 1);
        if (ret != -EIO) {
            break;
        }
        // ws2812_i2s 送完 START 才送 DRAIN，只有一顆 LED 時資料很短，中間被中斷（Wi-Fi 忙的時候很常見）
        // 就已經傳完，DRAIN 回 -EIO；I2S 也可能停在 ERROR，要 PREPARE 才回得來（其他狀態下沒有作用）。
        // 重送同一個顏色沒有副作用
        k_msleep(1);
        if (i2s_dev != NULL) {
            i2s_trigger(i2s_dev, I2S_DIR_TX, I2S_TRIGGER_PREPARE);
        }
    }
    k_mutex_unlock(&led_mutex);
    if (ret < 0) {
        LOG_WRN("LED write failed (%d)", ret);
    }
    return ret;
}

int led_set(uint8_t r, uint8_t g, uint8_t b)
{
    return led_set_raw(r * LED_MAX_BRIGHTNESS / 255, g * LED_MAX_BRIGHTNESS / 255,
                       b * LED_MAX_BRIGHTNESS / 255);
}

int led_set_hex(uint32_t rgb)
{
    return led_set((rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff);
}

int led_set_base(uint32_t rgb)
{
    if (rgb != base_rgb) {
        prefs_save("led/base", &rgb, sizeof(rgb));
    }
    base_rgb = rgb;
    return led_set_hex(rgb);
}

uint32_t led_get_base(void)
{
    return base_rgb;
}

static void flash_end(struct k_work *work)
{
    led_set_hex(base_rgb);
}
static K_WORK_DELAYABLE_DEFINE(flash_work, flash_end);

int led_restore(void)
{
    k_work_cancel_delayable(&flash_work);
    return led_set_hex(base_rgb);
}

void led_flash(uint32_t rgb, uint32_t ms)
{
    led_set_hex(rgb);
    k_work_reschedule(&flash_work, K_MSEC(ms));
}

// 開機時恢復上次 Light app 設定的顏色
static int led_init(void)
{
    if (prefs_load("led/base", &base_rgb, sizeof(base_rgb)) == sizeof(base_rgb)) {
        led_restore();
    }
    else {
        base_rgb = 0;
    }
    return 0;
}

SYS_INIT(led_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
