#include "fg_app.h"
#include "ui.h"
#include "led.h"
#include "file.h"
#include "wlan.h"
#include "net_server.h"
#include <zephyr/fs/fs.h>
#include <zephyr/net/socket.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/*
 * 上傳檔案到 SD 卡：開著這個 app 時在 port 80 跑一個 HTTP server。
 *   瀏覽器打開 http://<IP>/ 選檔案，或 curl -T photo.jpg http://<IP>/photo/
 * PUT（或 POST）/<路徑> 寫到 SD 卡上的同一個路徑，資料夾不存在就建立。
 * 一次服務一個連線（keep-alive，同一個連線可以連續傳好幾個檔），全部在 run() 裡做。
 */

#define UPLOAD_PORT         80
#define UPLOAD_ACCENT_COLOR 0x43c59e
#define UPLOAD_PATH_MAX     96
#define UPLOAD_LINE_MAX     160     // 更長的 header 只看開頭，用不到的部分就丟掉
#define UPLOAD_CHUNK        1024
#define UPLOAD_BUDGET_MS    20      // 沒在傳檔時，每次 run 最多花這麼久等資料
#define UPLOAD_BODY_MS      150     // 傳檔中：TCP window 只有 3 KB，一直收才不會讓對方停下來等
#define UPLOAD_IDLE_MS      10000   // 連線這麼久沒動靜就關掉，讓下一個連線進來
#define UPLOAD_UI_MS        200
#define UPLOAD_DRAIN_MAX    (1024 * 1024)   // 比這大的 body 不讀完，直接斷線

// ST_DRAIN：已經決定好錯誤回應，先把對方送來的 body 讀完丟掉再回（直接斷線的話對方會收到 RST，
// 看不到錯誤訊息，而且主動斷線的連線要在 TIME_WAIT 佔著一個 socket）
typedef enum { ST_HEADER, ST_BODY, ST_DRAIN, ST_SEND } conn_state_t;
typedef enum { REQ_OTHER, REQ_GET, REQ_PUT } req_method_t;

typedef struct {
    lv_obj_t *url_label;
    lv_obj_t *status_label;
    lv_obj_t *file_label;
    lv_obj_t *bar;
    lv_obj_t *total_label;

    int listen_fd;
    int fd;
    conn_state_t state;
    int64_t last_activity;
    bool online;

    // request
    char line[UPLOAD_LINE_MAX];
    size_t line_len;
    bool first_line;
    req_method_t method;
    bool bad_path;
    bool has_length;
    bool expect_continue;
    bool keep_alive;
    char path[UPLOAD_PATH_MAX];     // 相對 FILE_ROOT，不含開頭的 '/'
    uint32_t body_len;
    uint32_t body_done;
    struct fs_file_t file;
    bool file_open;

    // response：header 寫在 out，body 指到常數字串
    char out[128];
    size_t out_len;
    size_t out_sent;
    const char *body;
    size_t body_out_len;
    size_t body_sent;
    bool close_after;

    int files;
    uint32_t bytes;
    int64_t last_ui;
    bool ui_dirty;
} self_t;

APP_FG_REGISTER(Upload, LV_SYMBOL_UPLOAD, app_upload_init, app_upload_run, app_upload_exit, 15, self_t);

static const char page[] =
    "<!doctype html><meta charset=utf-8><meta name=viewport content='width=device-width'>"
    "<title>Cube upload</title><style>"
    "body{font:16px sans-serif;background:#10141c;color:#eee;max-width:32em;margin:2em auto;padding:0 1em}"
    "input,button{font:inherit;margin:.3em 0}#log div{margin:.2em 0;color:#9ab}"
    "</style><h2>Upload to SD card</h2>"
    "<p>Folder <input id=dir value=photo> <small>(empty: card root)</small></p>"
    "<p><input type=file id=files multiple></p><p><button onclick=go()>Upload</button></p>"
    "<div id=log></div><script>"
    "function put(f,dir){const e=document.createElement('div');log.prepend(e);"
    "const p=(dir?dir.replace(/^\\/+|\\/+$/g,'')+'/':'')+f.name;"
    "return new Promise(r=>{const x=new XMLHttpRequest();"
    "x.open('PUT','/'+p.split('/').map(encodeURIComponent).join('/'));"
    "x.upload.onprogress=v=>e.textContent=p+' '+Math.round(v.loaded*100/v.total)+'%';"
    "x.onload=()=>{e.textContent=p+': '+x.responseText;r()};"
    "x.onerror=()=>{e.textContent=p+': connection failed';r()};x.send(f)})}"
    "async function go(){for(const f of files.files)await put(f,dir.value.trim())}"
    "</script>";

static void ui_status(const char *text)
{
    lv_label_set_text(self->status_label, text);
}

static void file_discard(void)
{
    if (!self->file_open) {
        return;
    }
    char full[sizeof(FILE_ROOT) + UPLOAD_PATH_MAX];

    fs_close(&self->file);
    self->file_open = false;
    snprintf(full, sizeof(full), FILE_ROOT "/%.*s", UPLOAD_PATH_MAX - 1, self->path);
    fs_unlink(full);
    LOG_WRN("Upload of %s not finished, removed", self->path);
}

static void request_reset(void)
{
    self->state = ST_HEADER;
    self->line_len = 0;
    self->first_line = true;
    self->method = REQ_OTHER;
    self->bad_path = false;
    self->has_length = false;
    self->expect_continue = false;
    self->keep_alive = true;
    self->path[0] = '\0';
    self->body_len = 0;
    self->body_done = 0;
}

static void conn_close(void)
{
    if (self->fd < 0) {
        return;
    }
    if (self->state == ST_BODY) {
        ui_status("Upload interrupted");
    }
    file_discard();
    zsock_close(self->fd);
    self->fd = -1;
}

static void respond(int code, const char *reason, const char *type, const char *body)
{
    uint32_t unread = (self->method == REQ_PUT) ? self->body_len - self->body_done : 0;

    self->body = body;
    self->body_out_len = strlen(body);
    self->body_sent = 0;
    // body 沒讀完不能接著處理下一個 request（剩下的資料會被當成 request），太大就只能斷線
    self->close_after = !self->keep_alive || unread > UPLOAD_DRAIN_MAX;
    self->out_len = snprintf(self->out, sizeof(self->out),
                             "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %u\r\n%s\r\n", code, reason,
                             type, (unsigned int)self->body_out_len, self->close_after ? "Connection: close\r\n" : "");
    self->out_sent = 0;
    self->state = (unread > 0 && !self->close_after) ? ST_DRAIN : ST_SEND;
}

static void respond_text(int code, const char *reason, const char *text)
{
    respond(code, reason, "text/plain", text);
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    c = tolower((unsigned char)c);
    return (c >= 'a' && c <= 'f') ? c - 'a' + 10 : -1;
}

// URL 的路徑解碼後存到 self->path。只收 FAT 能用、看得懂的字元，不准 ".." 跳出去
static bool path_set(const char *target, size_t len)
{
    size_t n = 0;

    while (len > 0 && *target == '/') {
        target++;
        len--;
    }
    for (size_t i = 0; i < len && target[i] != '?'; i++) {
        char c = target[i];
        if (c == '%' && i + 2 < len && hex_value(target[i + 1]) >= 0 && hex_value(target[i + 2]) >= 0) {
            c = (char)(hex_value(target[i + 1]) * 16 + hex_value(target[i + 2]));
            i += 2;
        }
        else if (c == '+') {
            c = ' ';
        }
        if (n + 1 >= sizeof(self->path) || !(isalnum((unsigned char)c) || strchr("/._- ()+", c))) {
            return false;
        }
        // 每一段都不能是空的或用 '.' 開頭（擋掉 "..", "//"）
        if ((n == 0 || self->path[n - 1] == '/') && (c == '/' || c == '.')) {
            return false;
        }
        self->path[n++] = c;
    }
    self->path[n] = '\0';
    return true;
}

static void request_line(const char *line)
{
    const char *sp1 = strchr(line, ' ');
    const char *sp2 = sp1 ? strchr(sp1 + 1, ' ') : NULL;

    if (sp1 == NULL || sp2 == NULL) {
        self->bad_path = true;
        self->keep_alive = false;
        return;
    }
    if (strncmp(line, "GET ", 4) == 0) {
        self->method = REQ_GET;
    }
    else if (strncmp(line, "PUT ", 4) == 0 || strncmp(line, "POST ", 5) == 0) {
        self->method = REQ_PUT;
    }
    self->bad_path = !path_set(sp1 + 1, sp2 - sp1 - 1);
    if (strcmp(sp2 + 1, "HTTP/1.0") == 0) {
        self->keep_alive = false;
    }
}

static void header_line(const char *line)
{
    const char *colon = strchr(line, ':');

    if (colon == NULL) {
        return;
    }
    size_t key_len = colon - line;
    const char *value = colon + 1;
    while (*value == ' ') {
        value++;
    }

    if (key_len == 14 && strncasecmp(line, "Content-Length", 14) == 0) {
        self->body_len = strtoul(value, NULL, 10);
        self->has_length = true;
    }
    else if (key_len == 6 && strncasecmp(line, "Expect", 6) == 0) {
        self->expect_continue = strncasecmp(value, "100-continue", 12) == 0;
    }
    else if (key_len == 10 && strncasecmp(line, "Connection", 10) == 0) {
        if (strncasecmp(value, "close", 5) == 0) {
            self->keep_alive = false;
        }
        else if (strncasecmp(value, "keep-alive", 10) == 0) {
            self->keep_alive = true;
        }
    }
    else if (key_len == 17 && strncasecmp(line, "Transfer-Encoding", 17) == 0) {
        self->has_length = false;   // chunked 不支援，當成沒有長度
        self->keep_alive = false;
    }
}

static int dirs_create(void)
{
    char full[sizeof(FILE_ROOT) + UPLOAD_PATH_MAX];

    for (char *slash = strchr(self->path, '/'); slash != NULL; slash = strchr(slash + 1, '/')) {
        snprintf(full, sizeof(full), FILE_ROOT "/%.*s", (int)(slash - self->path), self->path);
        if (!file_exists(full)) {
            int ret = fs_mkdir(full);
            if (ret < 0) {
                return ret;
            }
        }
    }
    return 0;
}

static void upload_finish(void)
{
    fs_close(&self->file);
    self->file_open = false;
    self->files++;
    self->bytes += self->body_len;
    LOG_INF("Saved %s (%u bytes)", self->path, (unsigned int)self->body_len);
    led_flash(UPLOAD_ACCENT_COLOR, 150);
    ui_status("Saved");
    self->ui_dirty = true;
    respond_text(201, "Created", "saved\n");
}

static void headers_done(void)
{
    char full[sizeof(FILE_ROOT) + UPLOAD_PATH_MAX];

    if (self->method == REQ_GET) {
        if (self->path[0] == '\0' || strcmp(self->path, "index.html") == 0) {
            respond(200, "OK", "text/html", page);
        }
        else {
            respond_text(404, "Not Found", "not found\n");
        }
        return;
    }
    if (self->method != REQ_PUT) {
        self->keep_alive = false;
        respond_text(405, "Method Not Allowed", "use PUT\n");
        return;
    }
    if (!self->has_length) {
        self->keep_alive = false;
        respond_text(411, "Length Required", "Content-Length required\n");
        return;
    }
    if (self->bad_path || self->path[0] == '\0' || self->path[strlen(self->path) - 1] == '/') {
        respond_text(400, "Bad Request", "bad file name (letters, digits and ._-+() only)\n");
        return;
    }

    snprintf(full, sizeof(full), FILE_ROOT "/%.*s", UPLOAD_PATH_MAX - 1, self->path);
    fs_file_t_init(&self->file);
    int ret = dirs_create();
    if (ret == 0) {
        ret = fs_open(&self->file, full, FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC);
    }
    if (ret < 0) {
        LOG_ERR("Cannot create %s (%d)", full, ret);
        respond_text(500, "Internal Server Error", "cannot create the file (SD card?)\n");
        return;
    }
    self->file_open = true;
    self->state = ST_BODY;
    lv_label_set_text(self->file_label, self->path);
    ui_status("Receiving");
    self->ui_dirty = true;

    if (self->expect_continue) {
        static const char cont[] = "HTTP/1.1 100 Continue\r\n\r\n";
        net_server_send(self->fd, cont, sizeof(cont) - 1);
    }
    if (self->body_len == 0) {
        upload_finish();
    }
}

// header 一行一行處理，回傳用掉的 bytes；header 結束就停下來，剩下的是 body
static size_t header_feed(const char *data, size_t len)
{
    size_t i = 0;

    while (i < len && self->state == ST_HEADER) {
        char c = data[i++];
        if (c != '\n') {
            if (c != '\r' && self->line_len < sizeof(self->line) - 1) {
                self->line[self->line_len++] = c;
            }
            continue;
        }
        self->line[self->line_len] = '\0';
        if (self->first_line) {
            // request 之間多出來的空行忽略
            if (self->line_len > 0) {
                self->first_line = false;
                request_line(self->line);
            }
        }
        else if (self->line_len == 0) {
            headers_done();
        }
        else {
            header_line(self->line);
        }
        self->line_len = 0;
    }
    return i;
}

static void body_feed(const char *data, size_t len)
{
    size_t want = MIN(len, self->body_len - self->body_done);
    ssize_t written = fs_write(&self->file, data, want);

    self->body_done += want;
    if (written != (ssize_t)want) {
        LOG_ERR("Write to %s failed (%d)", self->path, (int)written);
        file_discard();
        respond_text(500, "Internal Server Error", "write failed (card full?)\n");
        ui_status("Write failed");
        return;
    }
    self->ui_dirty = true;
    if (self->body_done == self->body_len) {
        upload_finish();
    }
}

// 回傳 false 表示連線已經關掉
static bool send_step(void)
{
    while (self->out_sent < self->out_len || self->body_sent < self->body_out_len) {
        ssize_t n;
        if (self->out_sent < self->out_len) {
            n = net_server_send(self->fd, self->out + self->out_sent, self->out_len - self->out_sent);
            self->out_sent += MAX(n, 0);
        }
        else {
            n = net_server_send(self->fd, self->body + self->body_sent, self->body_out_len - self->body_sent);
            self->body_sent += MAX(n, 0);
        }
        if (n < 0) {
            conn_close();
            return false;
        }
        if (n == 0) {
            return true;    // socket buffer 滿了，下次再送
        }
    }
    if (self->close_after) {
        conn_close();
        return false;
    }
    request_reset();
    return true;
}

static void conn_serve(void)
{
    char buf[UPLOAD_CHUNK];
    int64_t start = k_uptime_get();

    while (self->fd >= 0) {
        int64_t now = k_uptime_get();
        int64_t end = start + (self->state == ST_BODY ? UPLOAD_BODY_MS : UPLOAD_BUDGET_MS);
        if (now >= end) {
            return;
        }
        if (self->state == ST_SEND) {
            if (!send_step() || self->state == ST_SEND) {
                return;
            }
            continue;
        }

        ssize_t n = net_server_recv(self->fd, buf, sizeof(buf), (int)(end - now));
        if (n == -EAGAIN) {
            if (now - self->last_activity > UPLOAD_IDLE_MS) {
                LOG_INF("Client idle, closing");
                conn_close();
            }
            return;
        }
        if (n <= 0) {
            conn_close();
            return;
        }
        self->last_activity = k_uptime_get();

        size_t used = 0;
        while (used < (size_t)n && self->fd >= 0) {
            if (self->state == ST_HEADER) {
                used += header_feed(buf + used, n - used);
            }
            else if (self->state == ST_BODY) {
                size_t take = MIN((size_t)n - used, self->body_len - self->body_done);
                body_feed(buf + used, take);
                used += take;
            }
            else if (self->state == ST_DRAIN) {
                size_t take = MIN((size_t)n - used, self->body_len - self->body_done);
                self->body_done += take;
                used += take;
                if (self->body_done == self->body_len) {
                    self->state = ST_SEND;
                }
            }
            else {
                // 回應還沒送完就收到下一個 request（pipelining）：不支援，送完就斷線
                self->close_after = true;
                break;
            }
        }
    }
}

static void listen_update(bool online)
{
    if (online && self->listen_fd < 0) {
        self->listen_fd = net_server_listen(UPLOAD_PORT);
        if (self->listen_fd < 0) {
            LOG_ERR("Cannot listen on port %d (%d)", UPLOAD_PORT, self->listen_fd);
        }
    }
    else if (!online && self->listen_fd >= 0) {
        conn_close();
        zsock_close(self->listen_fd);
        self->listen_fd = -1;
    }
}

static void ui_update(void)
{
    if (self->online) {
        wlan_info_t info;
        wlan_info(&info);
        ui_label_set_fmt(self->url_label, "http://%s/", info.ip);
    }
    else {
        ui_label_set_fmt(self->url_label, "Wi-Fi offline");
    }

    if (self->state == ST_BODY && self->body_len > 0) {
        lv_bar_set_value(self->bar, (int32_t)((uint64_t)self->body_done * 100 / self->body_len), LV_ANIM_OFF);
    }
    else if (self->files > 0 && self->fd < 0) {
        lv_bar_set_value(self->bar, 100, LV_ANIM_OFF);
    }
    ui_label_set_fmt(self->total_label, "%d file%s, %u KB", self->files, self->files == 1 ? "" : "s",
                     (unsigned int)((self->bytes + 1023) / 1024));
}

static void app_upload_init(void *scr)
{
    ui_screen_init(scr);
    ui_title(scr, "Upload");
    self->listen_fd = -1;
    self->fd = -1;

    self->url_label = ui_label(scr, &lv_font_montserrat_18, 0xffffff);
    lv_obj_align(self->url_label, LV_ALIGN_CENTER, 0, -44);

    self->status_label = ui_label(scr, &lv_font_montserrat_14, UI_DIM_COLOR);
    lv_obj_align(self->status_label, LV_ALIGN_CENTER, 0, -12);

    self->file_label = ui_label(scr, &lv_font_montserrat_14, 0xffffff);
    lv_obj_set_width(self->file_label, 200);
    lv_label_set_long_mode(self->file_label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(self->file_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(self->file_label, LV_ALIGN_CENTER, 0, 14);

    self->bar = lv_bar_create(scr);
    lv_obj_set_size(self->bar, 180, 8);
    lv_obj_align(self->bar, LV_ALIGN_CENTER, 0, 38);
    lv_obj_set_style_bg_color(self->bar, lv_color_hex(0x1c2333), LV_PART_MAIN);
    lv_obj_set_style_bg_color(self->bar, lv_color_hex(UPLOAD_ACCENT_COLOR), LV_PART_INDICATOR);

    self->total_label = ui_label(scr, &lv_font_montserrat_14, UI_DIM_COLOR);
    lv_obj_align(self->total_label, LV_ALIGN_BOTTOM_MID, 0, -16);

    ui_status("Open the address in a browser");
    self->online = wlan_is_online();
    listen_update(self->online);
    ui_update();
}

static void app_upload_run(void *scr)
{
    int64_t now = k_uptime_get();
    bool online = wlan_is_online();

    if (online != self->online) {
        self->online = online;
        listen_update(online);
        ui_status(online ? "Open the address in a browser" : "Waiting for Wi-Fi");
        self->ui_dirty = true;
    }
    else if (online && self->listen_fd < 0) {
        listen_update(true);    // 上次失敗（例如 socket 用完），再試一次
    }

    // 瀏覽器常常同時開兩條連線，閒在兩個 request 之間的那條讓給新來的，不然新的要等到 idle 逾時
    bool between = self->fd >= 0 && self->state == ST_HEADER && self->first_line && self->line_len == 0;
    if (self->listen_fd >= 0 && (self->fd < 0 || between)) {
        int fd = net_server_accept(self->listen_fd);
        if (fd >= 0) {
            conn_close();
            self->fd = fd;
            self->last_activity = now;
            request_reset();
        }
    }
    if (self->fd >= 0) {
        conn_serve();
    }

    if (self->ui_dirty && now - self->last_ui >= UPLOAD_UI_MS) {
        self->ui_dirty = false;
        self->last_ui = now;
        ui_update();
    }
}

static void app_upload_exit(void *scr)
{
    conn_close();
    if (self->listen_fd >= 0) {
        zsock_close(self->listen_fd);
    }
}
