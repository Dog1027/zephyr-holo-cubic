# ZephyrHoloCubic

用 Zephyr RTOS 和 LVGL 做的 HoloCubic 風格小螢幕裝置：ESP32 驅動一塊 240x240 的 ST7789 螢幕，畫面透過上方的分光稜鏡立方體觀看，另有 SD 卡、MPU6050 姿態感測器和一顆 WS2812 RGB LED。

操作主要透過 USB 序列埠的 shell 指令，敲一下裝置可以當成簡單的「下一個」。

## 功能

開機後是橫向輪播的 Menu，左上角是時間（對時之後才顯示），右上角的 Wi-Fi 圖示上線時是白色。可以開啟下列 app：

| App | 說明 | 需要的 SD 卡內容 |
|---|---|---|
| Clock | 時鐘，網路對時；OK 或敲一下切換 12 / 24 小時制（重開機後保留） | `/config.txt`（Wi-Fi，時區） |
| Weather | 目前天氣（wttr.in）和三天預報。打開時才抓（約 9 秒），30 分鐘內的資料直接顯示，開著時每 30 分鐘更新；OK 或敲一下馬上更新 | `/config.txt`（Wi-Fi，地點） |
| Files | 瀏覽 SD 卡，資料夾排在前面 | |
| Anim | 播放 JPEG 序列動畫，循環播放 | `/anim/<名稱>/000.jpg …` |
| Photos | 相簿，可自動輪播 | `/photo/*.jpg` |
| Level | 水平儀，可設零點（重開機後保留） | |
| Dice | 骰子與抽籤，搖一搖或敲一下就擲 | `/lots.txt`（抽籤用） |
| Light | 用 RGB LED 當氣氛燈，顏色重開機後保留 | |
| Timer | 計時器，翻面朝下會暫停，離開 app 也會繼續計時 | |
| SysInfo | 運行時間、heap、SD 卡容量、各 thread 的 stack 用量 | |
| Network | 最近一分鐘的訊號強度曲線、SSID、IP、gateway、MAC；選 Scan 按 OK 掃描附近的網路 | `/config.txt`（Wi-Fi） |
| Test | 空的 app，給測試新功能用 | |

## 硬體

| 元件 | 介面 | 腳位 |
|---|---|---|
| ESP32 DevKitC | | 板子：`esp32_devkitc/esp32/procpu` |
| ST7789V 240x240 螢幕 | SPI3（MIPI DBI，DMA） | SCLK 18、MOSI 23、DC 2、RST 4、背光 5（低電位點亮） |
| SD 卡 | SPI2 | SCLK 14、MOSI 13、MISO 26、CS 15 |
| MPU6050 | I2C0，位址 0x68 | SDA 32、SCL 33（目前用內建上拉，建議加 4.7 kΩ 外部上拉） |
| WS2812 RGB LED | I2S0 | DATA 27 |

腳位定義都在 [app.overlay](app.overlay)。

## 開發環境

這個資料夾是 west workspace 裡的一個 application，預期的目錄結構：

```
zephyrproject/
├── .venv/              Python 虛擬環境（west 和轉檔工具用的 Pillow 都裝在這裡）
├── zephyr/             Zephyr 4.3
├── modules/
└── applications/
    └── cube/           這個專案
```

需要：

- Zephyr 4.3 與對應的 Zephyr SDK（目前用 0.17.4）
- Espressif 的 Wi-Fi binary blob：`west blobs fetch hal_espressif`
- CMake、Ninja、gperf
- [just](https://just.systems)

## 編譯與燒錄

```bash
just build      # 編譯，輸出在 build/
just flash      # 燒錄
just monitor    # 序列埠 console（Ctrl+] 離開）
just upload     # 編譯並燒錄
just run        # 編譯、燒錄並開啟 console
just config     # menuconfig
just test       # 透過序列埠自動開關每個 app，回報錯誤和 stack 用量（先關掉 monitor）
```

燒錄和 console 預設用 CP2102 的固定路徑（`/dev/serial/by-id/...CP2102...`），不受 ttyUSB 編號變動影響。換板子、燒錄速度或序列埠：`just board=<board> build`、`just baud=115200 flash`、`just port=/dev/ttyUSB0 flash`。

## 操作

在 `just monitor` 裡輸入指令：

```
app list              列出所有 app 和 index
app open Photos       開啟 app（名稱不分大小寫，也可以用 index）
app close             關閉 app，回到 Menu

ui next / ui prev     下一個 / 上一個
ui ok                 選取
ui back               返回 Menu
ui tap                等同敲一下裝置
ui remote             遙控模式

tool imu_get          讀取加速度
tool led_set R G B    直接設定 LED 顏色（0-255）

wlan set SSID [PSK]   存到 SD 卡的 /config.txt 並連線（SSID 有空白要加引號，開放網路不給 PSK）
wlan status           設定、連線狀態和 IP
wlan scan             列出附近的網路
wlan connect          用 SD 卡上的設定重新連線
wlan disconnect       斷線，之後不會自動重連，直到 wlan connect
wlan forget           從 /config.txt 刪掉 SSID / PSK 並斷線

ntp status            目前時間和時區
ntp sync              立刻重新對時

weather               目前天氣和三天預報
weather update        立刻更新

config list           顯示 /config.txt（密碼會遮住）
config get KEY        讀一個值
config set KEY VALUE  設定一個值（值有空白要加引號）
config unset KEY      刪掉一個 key
```

`ui remote` 進入遙控模式後，按鍵會直接控制裝置：

| 按鍵 | 功能 |
|---|---|
| 方向鍵，或 `w` `a` `s` `d` | 上一個 / 下一個 |
| `Enter` 或空白鍵 | 選取 |
| `e` | 等同敲一下裝置 |
| `q`、`Esc`、`Backspace` 或 `b` | 關閉 app，返回 Menu |
| `Ctrl+Q` 或 `Ctrl+C` | 離開遙控模式 |

**敲一下裝置**在 app 裡預設等於 `ui next`，在 Menu 不會有反應（避免誤觸換掉選擇）。部分 app 會改成更合適的用途：Photos 換下一張、Dice 擲骰、Level 設零點、Timer 在計時中暫停或繼續。

## 準備 SD 卡內容

SD 卡要格式化成 FAT32（不支援 exFAT）。韌體不會自動格式化，必要時在 shell 用 `fs mkfs`。

圖片與動畫必須是 **baseline JPEG**，尺寸不超過 240x240。一般軟體存出來的 JPEG 常是 progressive，板子會解不出來，請用附的轉檔工具：

```bash
# 照片：整個資料夾轉成 240x240，裁切填滿（contain 則是保留完整畫面加黑邊）
just img2jpg ~/Pictures out/photo
just img2jpg ~/Pictures out/photo 240x240 85 contain

# GIF 轉成動畫：輸出 000.jpg、001.jpg … 和記錄每格時間的 delay.txt
just gif2jpg cat.gif out/cat
just gif2jpg cat.gif out/cat 120x120
```

轉好後依照下面的結構複製到 SD 卡：

```
/photo/                 Photos 的照片
/anim/cat/              每個動畫一個資料夾
    000.jpg
    001.jpg
    delay.txt           每格毫秒數（選用，預設 100）
/lots.txt               抽籤清單，一行一項（字型只支援英數字）
/config.txt             設定檔（Wi-Fi、時區），格式見下面
```

動畫的編號不能有缺號，播放時讀不到下一格就會回到第一格。尺寸越小播放越順，120x120 的解碼量只有 240x240 的四分之一。

### 設定檔 /config.txt

開機時從 SD 卡的 `/config.txt` 讀取設定，可以在電腦上直接編輯，也可以在 shell 用 `wlan set` 或 `config set`：

```
# HoloCubic 設定
wifi_ssid=MyNetwork
wifi_psk=password
tz=+8
```

| Key | 說明 | 預設 |
|---|---|---|
| `wifi_ssid` | Wi-Fi 名稱，沒有就不連網路 | |
| `wifi_psk` | Wi-Fi 密碼，8 到 64 個字元（WPA2-PSK）；開放網路不要寫 | |
| `tz` | 時區（UTC 偏移），例如 `+8`、`-5`、`+5:30`；打開 Clock 時重新讀取 | `+8` |
| `ntp_server` | 對時用的 NTP server | `pool.ntp.org` |
| `weather_location` | 天氣的地點：城市名（`Taipei`、`New York`）或 `緯度,經度`；下次更新時生效 | 依 IP 判斷 |

- 一行一個 `key=value`，key 和值前後的空白會被忽略；`#` 開頭的行是註解。
- 用指令修改時會保留其他行和註解，檔案上限 1 KB。
- Wi-Fi 斷線或連不上時每 10 秒重試一次，只在第一次失敗時記 log。只支援 2.4 GHz。
- 連上網路後自動對時，之後每 6 小時再對一次；失敗時 15 秒後重試。

## 新增 app

1. 在 `src/apps/fg/` 新增檔案，例如 `app_uptime.c`：

   ```c
   #include "fg_app.h"
   #include "ui.h"

   // app 開著時的狀態，放在共用的 app_state_mem，每次打開前清成 0
   typedef struct {
       lv_obj_t *label;
       uint32_t shown;
   } self_t;

   APP_FG_REGISTER(Uptime, LV_SYMBOL_BELL, app_uptime_init, app_uptime_run, app_uptime_exit, 20,
                   self_t);

   static void app_uptime_init(void *scr)
   {
       ui_screen_init(scr);
       self->label = lv_label_create(scr);
       lv_obj_center(self->label);
       self->shown = UINT32_MAX;   // 0 以外的初始值要自己設
   }

   static void app_uptime_run(void *scr)
   {
       uint32_t sec = k_uptime_get_32() / 1000;
       if (sec != self->shown) {
           self->shown = sec;
           lv_label_set_text_fmt(self->label, "%u", sec);
       }
   }

   static void app_uptime_exit(void *scr)
   {
   }
   ```

2. 在 [src/apps/CMakeLists.txt](src/apps/CMakeLists.txt) 加上 `target_sources(app PRIVATE fg/app_uptime.c)`。

規則：

- **註冊參數**：名稱、Menu 圖示（`LV_SYMBOL_*`）、三個 callback、priority、state 型別。priority 決定在 Menu 的順序，不能是 0（保留給 Menu），每個 app 用不同的值。
- **狀態放在 `self_t`**：app 開著時要記住的東西（LVGL 物件、畫面狀態、小 buffer）都放進 `self_t`，用 `self->` 存取，不要寫成 `static` 變數。所有前景 app 共用同一塊記憶體（`CONFIG_APP_STATE_SIZE`，放在 DRAM region 1），框架在 `init` 之前把它清成 0，app 關閉後就交給下一個 app。沒有狀態的 app 用 `app_no_state_t`。
  - 0 以外的初始值要在 `init` 裡設定。
  - `self_t` 放不下時 build 會失敗，再調大 `CONFIG_APP_STATE_SIZE`。大 buffer（超過幾百 bytes、只在某個函式裡用）放 stack，不要放 `self_t`。
  - `exit` 之後不能再用 `self`：不能讓 timer、work 或其他 thread 拿著指向它的指標。
  - **要在 app 關閉後保留的東西才用 `static`**，並加註解說明原因。目前只有兩種：離開 app 還要繼續運作的狀態（Timer 的計時），以及只初始化一次的 `lv_style_t`（Dice、Light、Timer）。
- **`init`**：每次開啟 app 都會呼叫，畫面已經清空，物件要在這裡重新建立。
- **`run`**：每一幀呼叫一次（間隔不固定，最長約 50 ms），不能長時間阻塞。
- **`exit`**：離開時呼叫，用來釋放自己配置的資源；畫面上的物件由框架刪除。
- **只能在這三個 callback 裡呼叫 LVGL**，所有 LVGL 操作都在同一個 thread。
- **可選取的物件**要加進 `lv_group_get_default()`，並移除 `LV_OBJ_FLAG_SCROLLABLE`，選取時會收到 `LV_EVENT_CLICKED`。`ui_list_item()` 和 `ui_tap_area()` 已經處理好這些。
- **想自己處理 next / prev / ok / 敲擊**（例如換照片）：在 `init` 呼叫 `nav_set_handler()`，回傳 `true` 表示已處理；app 關閉時框架會自動清掉。
- **常用的共用函式**，先找這裡再自己寫：
  - `ui.h`：`ui_label()`（字型 + 顏色）、`ui_label_set_fmt()`（文字有變才更新）、`ui_list_row()`（右邊帶數值的列表項）。
  - `led.h`：`led_flash()` 亮一下再自動回到底色。
  - `file.h`：SD 卡路徑一律用 `FILE_ROOT "/..."`。
  - `net_time.h`：`net_time_wday_names` / `net_time_month_names`。
- **Log**：所有模組和 app 共用 `CONFIG_CUBE_LOG_LEVEL`（預設 3 = INFO）；app 上限 `CONFIG_APP_MAX`（預設 24）。

## 專案結構

```
app.overlay             硬體與腳位（devicetree）
prj.conf                Kconfig 設定
src/
  app_thread.c          LVGL 主迴圈，切換背景 / 前景 app
  app_mgr.c             app 切換事件佇列
  nav.c                 導航指令佇列（UART、敲擊）
  tool.c                shell 指令：app、ui、tool
  input.c               MPU6050 姿態角與敲擊偵測
  gui.c                 螢幕與背光初始化
  ui.c                  共用的 LVGL 元件（標題、清單）
  jpeg.c                JPEG 解碼後直接寫到螢幕
  led.c                 RGB LED 與基礎顏色
  prefs.c               斷電保存的設定（settings / NVS）
  file.c                SD 卡掛載
  sdconf.c              SD 卡上的設定檔 /config.txt（key=value）
  wlan.c                Wi-Fi 連線、掃描與 wlan 指令
  net_job.c             網路工作共用的背景 work queue
  net_time.c            SNTP 對時、時區與 ntp 指令
  weather.c             從 wttr.in 抓天氣（HTTP，邊收邊解析 JSON）與 weather 指令
  i2c_pullup.c          I2C 腳位的 RTC 內建上拉與開機時的 bus recovery
  lib/tjpgd/            TJpgDec（專案內的設定，見 tjpgdcnf.h）
  apps/bg/app_menu.c    Menu
  apps/fg/              各個 app
tools/                  圖片與動畫轉檔工具
```

## 注意事項

這些是開發過程中踩到的問題，修改相關設定前請先看：

- **畫面鏡像**：`app.overlay` 的 `mdac = <0x40>` 是左右鏡像，給透過稜鏡觀看用。如果看到的畫面上下顛倒，改用 `0x80` 並把 `y-offset` 設成 `80`；直接看螢幕則用 `0x00`。傾斜方向相反時用 `CONFIG_INPUT_ROLL_INVERT` / `CONFIG_INPUT_PITCH_INVERT`。
- **只能一條 SPI 開 DMA**：兩條 SPI 共用 ESP32 的 DMA 時脈，Zephyr 驅動會讓第二條初始化失敗（log 出現 "Could not enable DMA clock"），所以只有螢幕的 SPI3 開 DMA。
- **SPI 速度**：SD 卡的 MISO 在 GPIO 26，要經過 GPIO 矩陣，超過 20 MHz 讀不到正確資料。螢幕目前 30 MHz。
- **I2C 維持 100 kHz**：板子上沒有外部上拉電阻，靠 ESP32 約 45 kΩ 的內建上拉（`src/i2c_pullup.c`），400 kHz 時 MPU6050 會初始化失敗。GPIO32/33 是 RTC pad，pinctrl 的 `bias-pull-up` 對它們沒有作用，上拉要設在 RTC_IO。
- **重置後 IMU 讀不到**：ESP32 重置時 MPU6050 不會斷電，如果剛好卡在 I2C 讀取途中，它會一直拉著 SDA。開機時在 sensor 初始化之前先做一次 bus recovery 解決。
- **網路工作不能在 app 裡做**：`run()` 不能阻塞，DNS、SNTP、HTTP 這類會等待的工作要丟到 `net_job_queue()`，app 只讀結果。
- **只能用 HTTP**：TLS 需要的記憶體放不下，所以網路服務都用純 HTTP。回應太大時要邊收邊解析（例如 `weather.c` 的 40 KB JSON 只用 512 B 的 buffer）。
- **DRAM 幾乎用滿**：開了 Wi-Fi 之後 DRAM region 0 只剩不到 1 KB。為了放得下，LVGL draw buffer 從畫面的 20% 降到 10%（`CONFIG_LV_Z_VDB_SIZE`），system heap 固定 48 KB，Wi-Fi 和網路的 buffer 數也調少了（見 `prj.conf`）。大塊的 buffer 請加 `__noinit`，會放到還有空間的 DRAM region 1（JPEG 的 buffer 就是這樣放的）。
- **不要在 `SYS_INIT` 裡繪圖**：開了 LVGL flush thread 後，它要等所有 `SYS_INIT` 結束才會啟動，在那之前繪圖會讓開機卡死。第一次繪圖在 `app_thread` 裡的 `gui_start()`。
- **LVGL 記憶體**：Menu 會一直留在背景，加上前景 app 和切換動畫，32 KB 不夠，目前設為 64 KB。可以用 SysInfo 看實際用量。
- **字型**：內建的 Montserrat 不含中文。

## 授權

`src/lib/tjpgd/` 是 ChaN 的 TJpgDec，授權見 [src/lib/tjpgd/LICENSE.txt](src/lib/tjpgd/LICENSE.txt)。
