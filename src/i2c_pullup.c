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
#include <zephyr/drivers/i2c.h>
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

/*
 * ESP32 重置時 MPU6050 不會斷電。如果重置剛好發生在 I2C 讀取途中（input thread 每 10 ms
 * 讀一次），MPU6050 還在等時脈把那個 byte 送完，會一直拉著 SDA，下一次開機讀 chip ID 就失敗。
 * 在 sensor 初始化之前先做一次 bus recovery：送時脈直到 SDA 放開，再補一個 STOP。
 */
#define I2C0_RECOVER_PRIORITY 70
BUILD_ASSERT(CONFIG_I2C_INIT_PRIORITY < I2C0_RECOVER_PRIORITY &&
             I2C0_RECOVER_PRIORITY < CONFIG_SENSOR_INIT_PRIORITY,
             "I2C bus recovery must run between the I2C and the sensor init");

static int i2c0_recover(void)
{
    const struct device *i2c = DEVICE_DT_GET(DT_NODELABEL(i2c0));

    if (device_is_ready(i2c)) {
        (void)i2c_recover_bus(i2c);
    }
    return 0;
}

SYS_INIT(i2c0_recover, POST_KERNEL, I2C0_RECOVER_PRIORITY);
