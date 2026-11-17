#include "fg_app.h"
#include "ui.h"
#include "jpeg.h"
#include "wlan.h"
#include "net_server.h"
#include <zephyr/net/socket.h>
#include <zephyr/sys/byteorder.h>

/*
 * 投影：開著時在 CAST_PORT 收電腦送來的 JPEG（tools/cast.py），直接畫到螢幕。
 * 每一格是 4 bytes 的長度（big-endian）加上 baseline JPEG（最大 240x240），
 * 畫完回一個 byte（'K' 成功、'E' 解不出來），電腦收到才送下一格，不會越積越慢。
 * 沒有地方放整張圖，解碼時直接從 socket 讀，邊收邊畫。
 */

#define CAST_PORT               8080
#define CAST_FRAME_MAX          (64 * 1024)
#define CAST_TIMEOUT_MS         3000    // 一格傳到一半停這麼久就斷線
#define CAST_POLL_MS            20
#define CAST_START_DELAY_MS     50      // 跟 Photos 一樣，淡入動畫結束後才開始直接畫

typedef struct {
    lv_obj_t *addr_label;
    lv_obj_t *info_label;
    int listen_fd;
    int fd;
    bool online;
    bool drawing;           // 畫面目前是 JPEG，不是 LVGL
    bool failed;            // 這一格讀到一半出錯（逾時或斷線）
    uint32_t remain;        // 這一格還沒讀的 bytes
    int64_t start_ms;
    int64_t conn_ms;
    uint32_t frames;
    uint32_t bad_frames;
    jpeg_mem_t jpeg;    // 解碼用的工作區（約 27 KB），不用初始化
} self_t;

APP_FG_REGISTER(Cast, ICON_CAST, app_cast_init, app_cast_run, app_cast_exit, 16, self_t);

// 讀滿 len bytes（buf 為 NULL 時丟掉），逾時或斷線就停，回傳讀到的量
static size_t recv_full(uint8_t *buf, size_t len)
{
    uint8_t skip[64];
    size_t done = 0;

    while (done < len && !self->failed) {
        uint8_t *dst = buf ? buf + done : skip;
        size_t want = buf ? len - done : MIN(len - done, sizeof(skip));
        ssize_t n = net_server_recv(self->fd, dst, want, CAST_TIMEOUT_MS);
        if (n <= 0) {
            self->failed = true;
            break;
        }
        done += n;
    }
    return done;
}

// 給 jpeg_show_stream()：只能讀到這一格的結尾
static size_t frame_read(void *ctx, uint8_t *buf, size_t len)
{
    size_t n = recv_full(buf, MIN(len, self->remain));

    self->remain -= n;
    return n;
}

static void ui_update(void)
{
    if (!self->online) {
        lv_label_set_text(self->addr_label, "Wi-Fi offline");
    }
    else {
        wlan_info_t info;
        wlan_info(&info);
        ui_label_set_fmt(self->addr_label, "%s:%d", info.ip, CAST_PORT);
    }
    if (self->frames + self->bad_frames == 0) {
        lv_label_set_text(self->info_label, "Run on the computer:\njust cast <address> <images, GIF\nor --screen>");
    }
    else {
        ui_label_set_fmt(self->info_label, "Last cast: %u frames%s", (unsigned int)self->frames,
                         self->bad_frames ? ", some not baseline JPEG" : "");
    }
}

// 回到 LVGL 畫的說明畫面
static void conn_close(void)
{
    if (self->fd < 0) {
        return;
    }
    zsock_close(self->fd);
    self->fd = -1;

    int64_t ms = k_uptime_get() - self->conn_ms;
    LOG_INF("Cast ended: %u frames in %lld ms", (unsigned int)self->frames, ms);
    if (self->drawing) {
        self->drawing = false;
        lv_obj_invalidate(lv_screen_active());
    }
    ui_update();
}

static void frame_receive(void)
{
    uint8_t hdr[4];
    uint8_t ack;

    self->failed = false;
    if (recv_full(hdr, sizeof(hdr)) < sizeof(hdr)) {
        conn_close();
        return;
    }
    self->remain = sys_get_be32(hdr);
    if (self->remain == 0 || self->remain > CAST_FRAME_MAX) {
        LOG_WRN("Bad frame length %u", (unsigned int)self->remain);
        conn_close();
        return;
    }

    if (!self->drawing) {
        jpeg_begin(&self->jpeg);
        self->drawing = true;
    }
    int ret = jpeg_show_stream(frame_read, NULL);
    // 解碼器不一定讀到最後一個 byte（或是中途放棄），剩下的丟掉，下一格才對得上
    recv_full(NULL, self->remain);
    if (self->failed) {
        conn_close();
        return;
    }

    if (ret == 0) {
        self->frames++;
    }
    else {
        self->bad_frames++;
    }
    ack = ret == 0 ? 'K' : 'E';
    if (net_server_send(self->fd, &ack, 1) != 1) {
        conn_close();
    }
}

static void listen_update(void)
{
    if (self->online && self->listen_fd < 0) {
        self->listen_fd = net_server_listen(CAST_PORT);
        if (self->listen_fd < 0) {
            LOG_ERR("Cannot listen on port %d (%d)", CAST_PORT, self->listen_fd);
        }
    }
    else if (!self->online && self->listen_fd >= 0) {
        conn_close();
        zsock_close(self->listen_fd);
        self->listen_fd = -1;
    }
}

static void app_cast_init(void *scr)
{
    ui_screen_init(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(THEME_BLACK), 0);
    ui_title(scr, "Cast");
    self->listen_fd = -1;
    self->fd = -1;
    self->start_ms = k_uptime_get();

    self->addr_label = ui_label(scr, &lv_font_montserrat_18, THEME_TEXT);
    lv_obj_align(self->addr_label, LV_ALIGN_CENTER, 0, -20);

    self->info_label = ui_label(scr, &lv_font_montserrat_14, THEME_TEXT_DIM);
    lv_obj_set_style_text_align(self->info_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(self->info_label, LV_ALIGN_CENTER, 0, 34);

    self->online = wlan_is_online();
    listen_update();
    ui_update();
}

static void app_cast_run(void *scr)
{
    int64_t now = k_uptime_get();
    bool online = wlan_is_online();

    if (online != self->online) {
        self->online = online;
        listen_update();
        ui_update();
    }
    else if (online && self->listen_fd < 0) {
        listen_update();
    }

    // 淡入動畫期間 LVGL 還在畫，先不接
    if (self->listen_fd < 0 || now < self->start_ms + CONFIG_APP_THREAD_ANIM_DURATION_MS + CAST_START_DELAY_MS) {
        return;
    }

    if (self->fd < 0) {
        int fd = net_server_accept(self->listen_fd);
        if (fd < 0) {
            return;
        }
        self->fd = fd;
        self->conn_ms = now;
        self->frames = 0;
        self->bad_frames = 0;
        LOG_INF("Cast started");
    }

    // 下一格還沒來就稍微等一下，比讓 app thread 睡到下一次 run 反應快；對方斷線也會變成可讀
    struct zsock_pollfd pfd = {.fd = self->fd, .events = ZSOCK_POLLIN};
    if (zsock_poll(&pfd, 1, CAST_POLL_MS) <= 0) {
        return;
    }
    frame_receive();
}

static void app_cast_exit(void *scr)
{
    jpeg_end();
    if (self->fd >= 0) {
        zsock_close(self->fd);
    }
    if (self->listen_fd >= 0) {
        zsock_close(self->listen_fd);
    }
}
