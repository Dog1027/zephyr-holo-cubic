/*
 * I2C0 (MPU6050) 的 SDA / SCL 在 GPIO32 / GPIO33，板子上沒有外部 pull-up，
 * 只能靠 ESP32 內建約 45 kΩ 的 pull-up（所以只能跑 100 kHz）。
 *
 * GPIO32/33 是 RTC pad：pinctrl 的 bias-pull-up 只設 IO_MUX 的 FUN_PU，
 * 在 RTC pad 上沒有作用，pull-up 必須設在 RTC_IO（X32P_RUE / X32N_RUE）。
 * 舊版 I2C 驅動在 bus clear 時用 GPIO_PULL_UP 設定腳位，順便開了 RTC pull-up；
 * 新版改成重新套用 pinctrl，這個副作用就沒了，IMU 會讀不到 chip ID。
 *
 * 在 I2C 初始化前把 RTC pull-up 打開。overlay 的 sda-gpios / scl-gpios 也要帶
 * GPIO_PULL_UP，不然 bus recovery 用 GPIO API 重設腳位時會把 pull-up 關掉。
 */
#include <zephyr/init.h>
#include <driver/rtc_io.h>

#define I2C0_SDA_PAD 32
#define I2C0_SCL_PAD 33

static int i2c0_rtc_pullup(void)
{
    rtc_gpio_pullup_en(I2C0_SDA_PAD);
    rtc_gpio_pullup_en(I2C0_SCL_PAD);
    return 0;
}

// 只改 RTC_IO 的 pull 位元，不需要其他驅動；越早越好，讓 I2C 初始化時線已經是高電位
SYS_INIT(i2c0_rtc_pullup, PRE_KERNEL_2, 0);
