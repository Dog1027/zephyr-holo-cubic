#include "fg_app.h"
#include "ui.h"
#include "led.h"
#include "file.h"
#include "wlan.h"
#include "net_server.h"
#include <zephyr/fs/fs.h>
#include <zephyr/net/socket.h>
#include <zephyr/random/random.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/*
 * 網頁版的檔案管理：開著這個 app 時在 port 80 跑一個 HTTP server，管理 SD 卡上的檔案。
 *   GET /                 檔案管理網頁（唯一不用 PIN 的）
 *   GET /<資料夾>/?list    資料夾內容，JSON：[{"n":"名稱","d":0 或 1,"s":大小}, ...]
 *   GET /<檔案>            下載
 *   PUT /<檔案>            上傳（POST 也可以），資料夾不存在就建立
 *   PUT /<資料夾>/         建立資料夾
 *   DELETE /<路徑>         刪除檔案或空的資料夾
 * 每次打開 app 產生一組 4 位數 PIN 顯示在螢幕上，用 cookie "pin=1234"（網頁）、
 * header "X-Pin: 1234" 或 "?pin=1234" 帶上來；連錯 SHARE_PIN_TRIES 次鎖一段時間。
 * 一次服務一個連線（keep-alive），全部在 run() 裡做。
 */

#define SHARE_PORT          80
#define SHARE_ACCENT_COLOR  0x43c59e
#define SHARE_PATH_MAX      96
#define SHARE_LINE_MAX      160     // 更長的 header 只看開頭，用不到的部分就丟掉
#define SHARE_CHUNK         1024
#define SHARE_NAME_MAX      100     // 列表裡更長的檔名不列出來
#define SHARE_BUDGET_MS     20      // 沒在傳檔時，每次 run 最多花這麼久等資料
#define SHARE_BODY_MS       150     // 傳檔中：TCP window 只有 3 KB，一直收送才不會讓對方停下來等
#define SHARE_IDLE_MS       10000   // 連線這麼久沒動靜就關掉，讓下一個連線進來
#define SHARE_UI_MS         200
#define SHARE_CLOSING_MS    1000    // 回應送完後等對方斷線，最多這麼久
#define SHARE_DRAIN_MAX     (1024 * 1024)   // 比這大的 body 不讀完，直接斷線
#define SHARE_PIN_TRIES     5
#define SHARE_PIN_LOCK_MS   30000

// ST_DRAIN：已經決定好錯誤回應，先把對方送來的 body 讀完丟掉再回（直接斷線的話對方會收到 RST，
// 看不到錯誤訊息，而且主動斷線的連線要在 TIME_WAIT 佔著一個 socket）
// ST_CLOSING：回應送完、這條連線不再用了，等對方先斷線（自己先關的話又是一個 TIME_WAIT）
typedef enum { ST_HEADER, ST_BODY, ST_DRAIN, ST_SEND, ST_CLOSING } conn_state_t;
typedef enum { REQ_OTHER, REQ_GET, REQ_PUT, REQ_DELETE } req_method_t;
// 回應的內容：常數字串、檔案（下載）、邊讀資料夾邊產生的 JSON
typedef enum { BODY_CONST, BODY_FILE, BODY_LIST } body_kind_t;
typedef enum { FILE_CLOSED, FILE_UPLOAD, FILE_DOWNLOAD } file_mode_t;

typedef struct {
    lv_obj_t *url_label;
    lv_obj_t *pin_label;
    lv_obj_t *status_label;
    lv_obj_t *file_label;
    lv_obj_t *bar;
    lv_obj_t *total_label;

    int listen_fd;
    int fd;
    conn_state_t state;
    int64_t last_activity;
    bool online;

    int pin;
    int pin_fails;
    int64_t pin_lock_until;

    // request
    char line[SHARE_LINE_MAX];
    size_t line_len;
    bool first_line;
    req_method_t method;
    bool bad_path;
    bool dir_slash;                 // 路徑用 '/' 結尾
    bool want_list;                 // ?list
    bool pin_given;
    bool pin_ok;
    bool has_length;
    bool expect_continue;
    bool keep_alive;
    char path[SHARE_PATH_MAX];      // 相對 FILE_ROOT，頭尾都沒有 '/'
    uint32_t body_len;
    uint32_t body_done;
    struct fs_file_t file;
    file_mode_t file_mode;

    // response：header（和 BODY_LIST 的每一塊）寫在 out
    char out[2 * SHARE_NAME_MAX + 64];
    size_t out_len;
    size_t out_sent;
    body_kind_t body_kind;
    const char *body;               // BODY_CONST
    size_t body_out_len;
    size_t body_sent;
    uint32_t file_size;             // BODY_FILE
    uint32_t file_left;
    struct fs_dir_t dir;            // BODY_LIST
    bool dir_open;
    bool list_first;
    bool list_done;
    bool close_after;

    int uploads;
    int downloads;
    int64_t last_ui;
    bool ui_dirty;
} self_t;

APP_FG_REGISTER(Share, LV_SYMBOL_DRIVE, app_share_init, app_share_run, app_share_exit, 15, self_t);

static const char page[] =
    "<!doctype html><meta charset=utf-8><meta name=viewport content='width=device-width'>"
    "<title>Cube files</title><style>"
    "body{font:16px sans-serif;background:#10141c;color:#eee;max-width:40em;margin:1.5em auto;padding:0 1em}"
    "a{color:#6cf;text-decoration:none;cursor:pointer}td{padding:.25em .8em .25em 0}"
    "td+td{color:#9ab;text-align:right}button,input{font:inherit}#log div{color:#9ab;margin:.2em 0}"
    "#drop{border:1px dashed #456;padding:.8em;margin:.8em 0}"
    "</style><h2 id=cwd></h2>"
    "<div id=drop><input type=file id=pick multiple> or drop files here "
    "<button onclick=mk()>New folder</button></div>"
    "<table id=tb></table><div id=log></div><script>"
    "let cwd='';const $=i=>document.getElementById(i);"
    "const enc=p=>p.split('/').filter(x=>x).map(x=>'/'+encodeURIComponent(x)).join('');"
    "const sub=n=>(cwd?cwd+'/':'')+n;"
    "function pin(ask){if(ask||!/pin=\\d{4}/.test(document.cookie)){"
    "const v=prompt('PIN shown on the cube');if(!v)return false;"
    "document.cookie='pin='+v.trim()+';path=/;SameSite=Strict'}return true}"
    "async function api(method,url){for(;;){const r=await fetch(url,{method});"
    "if(r.status!=401||!pin(true))return r}}"
    "function say(t){const e=document.createElement('div');e.textContent=t;$('log').prepend(e);return e}"
    "const size=n=>n<1024?n+' B':n<1048576?(n/1024).toFixed(1)+' KB':(n/1048576).toFixed(1)+' MB';"
    "function row(name,info,click,href){const tr=$('tb').insertRow(),a=document.createElement('a');"
    "a.textContent=name;if(click)a.onclick=click;else{a.href=href;a.target='_blank'}"
    "tr.insertCell().append(a);tr.insertCell().textContent=info;return tr}"
    "async function ls(){const r=await api('GET',enc(cwd)+'/?list');"
    "if(!r.ok){say('/'+cwd+': '+await r.text());return}"
    "const l=await r.json();l.sort((a,b)=>b.d-a.d||a.n.localeCompare(b.n));"
    "$('cwd').textContent='SD card /'+cwd;$('tb').textContent='';"
    "if(cwd)row('..','',()=>{cwd=cwd.split('/').slice(0,-1).join('/');ls()});"
    "for(const e of l){const p=sub(e.n);"
    "const tr=row(e.n+(e.d?'/':''),e.d?'':size(e.s),e.d?()=>{cwd=p;ls()}:null,enc(p));"
    "const b=document.createElement('button');b.textContent='Delete';"
    "b.onclick=async()=>{if(confirm('Delete '+p+'?')){const r=await api('DELETE',enc(p));"
    "say(p+': '+await r.text());ls()}};tr.insertCell().append(b)}}"
    "function put(f){const p=sub(f.name),e=say(p);return new Promise(r=>{const x=new XMLHttpRequest();"
    "x.open('PUT',enc(p));x.upload.onprogress=v=>e.textContent=p+' '+Math.round(v.loaded*100/v.total)+'%';"
    "x.onload=()=>{e.textContent=p+': '+x.responseText;r()};"
    "x.onerror=()=>{e.textContent=p+': connection failed';r()};x.send(f)})}"
    "async function up(files){for(const f of files)await put(f);ls()}"
    "async function mk(){const n=prompt('Folder name');if(!n)return;"
    "const r=await api('PUT',enc(sub(n))+'/');say(sub(n)+': '+await r.text());ls()}"
    "$('pick').onchange=()=>up($('pick').files);$('drop').ondragover=e=>e.preventDefault();"
    "$('drop').ondrop=e=>{e.preventDefault();up(e.dataTransfer.files)};"
    "if(pin())ls()"
    "</script>";

static void ui_status(const char *text)
{
    lv_label_set_text(self->status_label, text);
}

static void full_path(char full[sizeof(FILE_ROOT) + SHARE_PATH_MAX])
{
    snprintf(full, sizeof(FILE_ROOT) + SHARE_PATH_MAX, FILE_ROOT "/%.*s", SHARE_PATH_MAX - 1, self->path);
}

// 關掉手上的檔案和資料夾；沒傳完的上傳檔案刪掉
static void files_close(void)
{
    if (self->file_mode != FILE_CLOSED) {
        fs_close(&self->file);
        if (self->file_mode == FILE_UPLOAD) {
            char full[sizeof(FILE_ROOT) + SHARE_PATH_MAX];

            full_path(full);
            fs_unlink(full);
            LOG_WRN("Upload of %s not finished, removed", self->path);
        }
        self->file_mode = FILE_CLOSED;
    }
    if (self->dir_open) {
        fs_closedir(&self->dir);
        self->dir_open = false;
    }
}

static void request_reset(void)
{
    self->state = ST_HEADER;
    self->line_len = 0;
    self->first_line = true;
    self->method = REQ_OTHER;
    self->bad_path = false;
    self->dir_slash = false;
    self->want_list = false;
    self->pin_given = false;
    self->pin_ok = false;
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
    if (self->file_mode != FILE_CLOSED) {
        ui_status("Transfer interrupted");
    }
    files_close();
    zsock_close(self->fd);
    self->fd = -1;
}

// 回應的 header 寫進 out；extra 是多加的 header（每行用 \r\n 結尾）
static void respond_begin(int code, const char *reason, const char *type, const char *extra)
{
    uint32_t unread = (self->method == REQ_PUT) ? self->body_len - self->body_done : 0;

    // body 沒讀完不能接著處理下一個 request（剩下的資料會被當成 request），太大就只能斷線
    self->close_after = !self->keep_alive || unread > SHARE_DRAIN_MAX;
    self->out_len = snprintf(self->out, sizeof(self->out), "HTTP/1.1 %d %s\r\nContent-Type: %s\r\n%s%s\r\n", code,
                             reason, type, extra, self->close_after ? "Connection: close\r\n" : "");
    self->out_sent = 0;
    self->state = (unread > 0 && !self->close_after) ? ST_DRAIN : ST_SEND;
}

static void respond(int code, const char *reason, const char *type, const char *body)
{
    char extra[32];

    self->body_kind = BODY_CONST;
    self->body = body;
    self->body_out_len = strlen(body);
    self->body_sent = 0;
    snprintf(extra, sizeof(extra), "Content-Length: %u\r\n", (unsigned int)self->body_out_len);
    respond_begin(code, reason, type, extra);
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

// s 開頭是不是正確的 4 位數 PIN
static void pin_check(const char *s)
{
    int value = 0;

    for (int i = 0; i < 4; i++) {
        if (!isdigit((unsigned char)s[i])) {
            return;
        }
        value = value * 10 + (s[i] - '0');
    }
    self->pin_given = true;
    if (value == self->pin && !isdigit((unsigned char)s[4])) {
        self->pin_ok = true;
    }
}

// URL 的路徑解碼後存到 self->path。只收 FAT 能用、看得懂的字元，不准 ".." 跳出去
static bool path_set(const char *target, size_t len)
{
    size_t n = 0;

    while (len > 0 && *target == '/') {
        target++;
        len--;
    }
    for (size_t i = 0; i < len; i++) {
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
    if (n > 0 && self->path[n - 1] == '/') {
        self->dir_slash = true;
        n--;
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
    else if (strncmp(line, "DELETE ", 7) == 0) {
        self->method = REQ_DELETE;
    }

    const char *target = sp1 + 1;
    const char *query = memchr(target, '?', sp2 - target);
    const char *path_end = query ? query : sp2;

    if (path_end == target || path_end[-1] == '/') {
        self->dir_slash = true;     // "/" 本身也算
    }
    self->bad_path = !path_set(target, path_end - target);
    for (const char *q = query; q != NULL && q < sp2; q = memchr(q, '&', sp2 - q)) {
        q++;
        if (strncmp(q, "list", 4) == 0) {
            self->want_list = true;
        }
        else if (strncmp(q, "pin=", 4) == 0) {
            pin_check(q + 4);
        }
    }
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
    else if (key_len == 6 && strncasecmp(line, "Cookie", 6) == 0) {
        const char *pin = strstr(value, "pin=");
        if (pin != NULL) {
            pin_check(pin + 4);
        }
    }
    else if (key_len == 5 && strncasecmp(line, "X-Pin", 5) == 0) {
        pin_check(value);
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

// 建立 path 裡每一層還不存在的資料夾；last 為 false 時不含最後一段（那是檔名）
static int dirs_create(bool last)
{
    char full[sizeof(FILE_ROOT) + SHARE_PATH_MAX];
    char *slash = self->path;

    while (true) {
        slash = strchr(slash, '/');
        if (slash == NULL && !last) {
            return 0;
        }
        int len = slash ? (int)(slash - self->path) : (int)strlen(self->path);
        snprintf(full, sizeof(full), FILE_ROOT "/%.*s", len, self->path);
        if (!file_exists(full)) {
            int ret = fs_mkdir(full);
            if (ret < 0) {
                return ret;
            }
        }
        if (slash == NULL) {
            return 0;
        }
        slash++;
    }
}

static void upload_finish(void)
{
    fs_close(&self->file);
    self->file_mode = FILE_CLOSED;
    self->uploads++;
    LOG_INF("Saved %s (%u bytes)", self->path, (unsigned int)self->body_len);
    led_flash(SHARE_ACCENT_COLOR, 150);
    ui_status("Saved");
    self->ui_dirty = true;
    respond_text(201, "Created", "saved\n");
}

static const char *content_type(const char *path)
{
    const char *dot = strrchr(path, '.');

    if (dot == NULL) {
        return "application/octet-stream";
    }
    if (strcasecmp(dot, ".jpg") == 0 || strcasecmp(dot, ".jpeg") == 0) {
        return "image/jpeg";
    }
    if (strcasecmp(dot, ".png") == 0) {
        return "image/png";
    }
    if (strcasecmp(dot, ".gif") == 0) {
        return "image/gif";
    }
    if (strcasecmp(dot, ".txt") == 0) {
        return "text/plain";
    }
    return "application/octet-stream";
}

static void get_list(const char *full)
{
    fs_dir_t_init(&self->dir);
    if (fs_opendir(&self->dir, full) < 0) {
        respond_text(404, "Not Found", "no such folder\n");
        return;
    }
    self->dir_open = true;
    self->list_first = true;
    self->list_done = false;
    self->body_kind = BODY_LIST;
    // 不知道總長度，用 chunked 才能維持 keep-alive
    respond_begin(200, "OK", "application/json", "Transfer-Encoding: chunked\r\nCache-Control: no-store\r\n");
}

static void get_file(const char *full)
{
    struct fs_dirent entry;
    char extra[40];

    if (fs_stat(full, &entry) < 0) {
        respond_text(404, "Not Found", "not found\n");
        return;
    }
    if (entry.type != FS_DIR_ENTRY_FILE) {
        respond_text(400, "Bad Request", "that is a folder\n");
        return;
    }
    fs_file_t_init(&self->file);
    if (fs_open(&self->file, full, FS_O_READ) < 0) {
        respond_text(500, "Internal Server Error", "cannot open the file\n");
        return;
    }
    self->file_mode = FILE_DOWNLOAD;
    self->file_size = entry.size;
    self->file_left = entry.size;
    self->body_kind = BODY_FILE;
    snprintf(extra, sizeof(extra), "Content-Length: %u\r\n", (unsigned int)entry.size);
    respond_begin(200, "OK", content_type(self->path), extra);

    lv_label_set_text(self->file_label, self->path);
    ui_status("Sending");
    self->ui_dirty = true;
}

static void put_file(const char *full)
{
    fs_file_t_init(&self->file);
    int ret = dirs_create(false);
    if (ret == 0) {
        ret = fs_open(&self->file, full, FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC);
    }
    if (ret < 0) {
        LOG_ERR("Cannot create %s (%d)", full, ret);
        respond_text(500, "Internal Server Error", "cannot create the file (SD card?)\n");
        return;
    }
    self->file_mode = FILE_UPLOAD;
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

// 先自己檢查再刪：fs_unlink 失敗時檔案系統會印 error log
static void delete_path(const char *full)
{
    struct fs_dirent entry;

    if (fs_stat(full, &entry) < 0) {
        respond_text(404, "Not Found", "not found\n");
        return;
    }
    if (entry.type == FS_DIR_ENTRY_DIR) {
        struct fs_dir_t dir;
        bool empty = false;

        fs_dir_t_init(&dir);
        if (fs_opendir(&dir, full) == 0) {
            empty = fs_readdir(&dir, &entry) == 0 && entry.name[0] == '\0';
            fs_closedir(&dir);
        }
        if (!empty) {
            respond_text(409, "Conflict", "the folder is not empty\n");
            return;
        }
    }
    if (fs_unlink(full) < 0) {
        respond_text(500, "Internal Server Error", "cannot delete\n");
        return;
    }
    LOG_INF("Deleted %s", self->path);
    lv_label_set_text(self->file_label, self->path);
    ui_status("Deleted");
    respond_text(200, "OK", "deleted\n");
}

// 回傳 false 表示已經回了錯誤
static bool pin_required(void)
{
    int64_t now = k_uptime_get();

    if (now < self->pin_lock_until) {
        respond_text(429, "Too Many Requests", "too many wrong PINs, wait 30 seconds\n");
        return false;
    }
    if (self->pin_ok) {
        self->pin_fails = 0;
        return true;
    }
    if (self->pin_given && ++self->pin_fails >= SHARE_PIN_TRIES) {
        self->pin_fails = 0;
        self->pin_lock_until = now + SHARE_PIN_LOCK_MS;
        LOG_WRN("%d wrong PINs, locked for %d s", SHARE_PIN_TRIES, SHARE_PIN_LOCK_MS / 1000);
        ui_status("Wrong PIN, locked 30 s");
    }
    respond_text(401, "Unauthorized", "PIN required (shown on the cube)\n");
    return false;
}

static void headers_done(void)
{
    char full[sizeof(FILE_ROOT) + SHARE_PATH_MAX];
    bool root = self->path[0] == '\0';

    // 建資料夾（結尾是 '/'）沒有 body，不用帶長度
    if (self->method == REQ_PUT && !self->has_length && !self->dir_slash) {
        self->keep_alive = false;
        respond_text(411, "Length Required", "Content-Length required\n");
        return;
    }
    if (self->method == REQ_GET && root && !self->want_list && !self->bad_path) {
        respond(200, "OK", "text/html", page);
        return;
    }
    if (!pin_required()) {
        return;
    }
    if (self->bad_path) {
        respond_text(400, "Bad Request", "bad name (letters, digits, space and ._-+() only)\n");
        return;
    }

    full_path(full);
    switch (self->method) {
    case REQ_GET:
        if (self->want_list) {
            get_list(root ? FILE_ROOT : full);
        }
        else {
            get_file(full);
        }
        break;
    case REQ_PUT:
        if (root) {
            respond_text(400, "Bad Request", "no file name\n");
        }
        else if (self->dir_slash) {
            if (dirs_create(true) == 0) {
                respond_text(201, "Created", "folder ready\n");
            }
            else {
                respond_text(500, "Internal Server Error", "cannot create the folder\n");
            }
        }
        else {
            put_file(full);
        }
        break;
    case REQ_DELETE:
        if (root) {
            respond_text(400, "Bad Request", "cannot delete the card root\n");
        }
        else {
            delete_path(full);
        }
        break;
    default:
        self->keep_alive = false;
        respond_text(405, "Method Not Allowed", "GET, PUT or DELETE\n");
        break;
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
        files_close();
        respond_text(500, "Internal Server Error", "write failed (card full?)\n");
        ui_status("Write failed");
        return;
    }
    self->ui_dirty = true;
    if (self->body_done == self->body_len) {
        upload_finish();
    }
}

// 下一塊 JSON（chunked 的一個 chunk）寫進 out：一個檔案一塊，最後是結尾
static void list_next(void)
{
    struct fs_dirent entry;
    char item[2 * SHARE_NAME_MAX + 48];
    const char *open = self->list_first ? "[" : ",";

    while (fs_readdir(&self->dir, &entry) == 0 && entry.name[0] != '\0') {
        size_t n = 0;
        size_t name_len = strlen(entry.name);

        if (name_len > SHARE_NAME_MAX) {
            continue;
        }
        for (size_t i = 0; i < name_len; i++) {
            unsigned char c = entry.name[i];
            if (c == '"' || c == '\\') {
                item[n++] = '\\';
            }
            // 字型和 FAT 的編碼都不是 UTF-8，非 ASCII 的字換成 '?'，JSON 才不會壞掉
            item[n++] = (c < 0x20 || c >= 0x7f) ? '?' : c;
        }
        item[n] = '\0';
        // item 接在自己後面組不出來，直接算好 chunk 長度寫進 out
        int body = snprintf(NULL, 0, "%s{\"n\":\"%s\",\"d\":%d,\"s\":%u}", open, item,
                            entry.type == FS_DIR_ENTRY_DIR, (unsigned int)entry.size);
        self->out_len = snprintf(self->out, sizeof(self->out), "%x\r\n%s{\"n\":\"%s\",\"d\":%d,\"s\":%u}\r\n", body, open,
                                 item, entry.type == FS_DIR_ENTRY_DIR, (unsigned int)entry.size);
        self->out_sent = 0;
        self->list_first = false;
        return;
    }

    fs_closedir(&self->dir);
    self->dir_open = false;
    self->list_done = true;
    self->out_len = snprintf(self->out, sizeof(self->out), "%s0\r\n\r\n", self->list_first ? "2\r\n[]\r\n" : "1\r\n]\r\n");
    self->out_sent = 0;
}

// 送出回應，buf 是借來讀檔用的。回傳 false 表示 socket 滿了或連線已經關掉，這次不用再試
static bool send_step(char *buf, size_t buf_size)
{
    while (true) {
        ssize_t n;

        if (self->out_sent < self->out_len) {
            n = net_server_send(self->fd, self->out + self->out_sent, self->out_len - self->out_sent);
            self->out_sent += MAX(n, 0);
        }
        else if (self->body_kind == BODY_CONST && self->body_sent < self->body_out_len) {
            n = net_server_send(self->fd, self->body + self->body_sent, self->body_out_len - self->body_sent);
            self->body_sent += MAX(n, 0);
        }
        else if (self->body_kind == BODY_FILE && self->file_left > 0) {
            ssize_t got = fs_read(&self->file, buf, MIN(buf_size, self->file_left));
            if (got <= 0) {
                LOG_ERR("Read of %s failed (%d)", self->path, (int)got);
                conn_close();   // 長度已經送出去了，只能斷線讓對方知道沒傳完
                return false;
            }
            n = net_server_send(self->fd, buf, got);
            if (n >= 0 && n < got) {
                // 沒送完的部分退回去，下次再讀
                fs_seek(&self->file, -(off_t)(got - n), FS_SEEK_CUR);
            }
            self->file_left -= MAX(n, 0);
            self->ui_dirty = true;
        }
        else if (self->body_kind == BODY_LIST && !self->list_done) {
            list_next();
            continue;
        }
        else {
            break;
        }

        if (n < 0) {
            conn_close();
            return false;
        }
        if (n == 0) {
            return false;
        }
    }

    if (self->file_mode == FILE_DOWNLOAD) {
        fs_close(&self->file);
        self->file_mode = FILE_CLOSED;
        self->downloads++;
        LOG_INF("Sent %s (%u bytes)", self->path, (unsigned int)self->file_size);
        ui_status("Sent");
        self->ui_dirty = true;
    }
    if (self->close_after) {
        self->state = ST_CLOSING;
        self->last_activity = k_uptime_get();
        return false;
    }
    request_reset();
    return true;
}

static void conn_serve(void)
{
    char buf[SHARE_CHUNK];
    int64_t start = k_uptime_get();

    while (self->fd >= 0) {
        int64_t now = k_uptime_get();
        bool transfer = self->file_mode != FILE_CLOSED;
        int64_t end = start + (transfer ? SHARE_BODY_MS : SHARE_BUDGET_MS);
        if (now >= end) {
            return;
        }
        if (self->state == ST_SEND) {
            if (send_step(buf, sizeof(buf))) {
                continue;
            }
            if (self->fd < 0 || !transfer) {
                return;
            }
            // 下載中 socket 滿了：等它空出來繼續送，不要睡到下一次 run
            struct zsock_pollfd pfd = {.fd = self->fd, .events = ZSOCK_POLLOUT};
            if (zsock_poll(&pfd, 1, (int)(end - now)) <= 0) {
                return;
            }
            self->last_activity = k_uptime_get();
            continue;
        }

        ssize_t n = net_server_recv(self->fd, buf, sizeof(buf), (int)(end - now));
        if (self->state == ST_CLOSING) {
            // 收到什麼都不理，對方斷線（n == 0）、出錯或等太久就關
            if (n != -EAGAIN || now - self->last_activity > SHARE_CLOSING_MS) {
                conn_close();
            }
            return;
        }
        if (n == -EAGAIN) {
            if (now - self->last_activity > SHARE_IDLE_MS) {
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
        self->listen_fd = net_server_listen(SHARE_PORT);
        if (self->listen_fd < 0) {
            LOG_ERR("Cannot listen on port %d (%d)", SHARE_PORT, self->listen_fd);
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

    if (self->file_mode == FILE_UPLOAD && self->body_len > 0) {
        lv_bar_set_value(self->bar, (int32_t)((uint64_t)self->body_done * 100 / self->body_len), LV_ANIM_OFF);
    }
    else if (self->file_mode == FILE_DOWNLOAD && self->file_size > 0) {
        lv_bar_set_value(self->bar, (int32_t)((uint64_t)(self->file_size - self->file_left) * 100 / self->file_size),
                         LV_ANIM_OFF);
    }
    else if (self->uploads + self->downloads > 0) {
        lv_bar_set_value(self->bar, 100, LV_ANIM_OFF);
    }
    ui_label_set_fmt(self->total_label, "Up %d   Down %d", self->uploads, self->downloads);
}

static void app_share_init(void *scr)
{
    ui_screen_init(scr);
    ui_title(scr, "Share");
    self->listen_fd = -1;
    self->fd = -1;
    self->pin = sys_rand32_get() % 10000;
    // 序列埠要接線才看得到，跟看螢幕一樣要人在旁邊；自動測試靠這行拿 PIN
    LOG_INF("PIN %04d", self->pin);

    self->url_label = ui_label(scr, &lv_font_montserrat_18, 0xffffff);
    lv_obj_align(self->url_label, LV_ALIGN_CENTER, 0, -62);

    self->pin_label = ui_label(scr, &lv_font_montserrat_48, SHARE_ACCENT_COLOR);
    lv_obj_set_style_text_letter_space(self->pin_label, 4, 0);
    lv_obj_align(self->pin_label, LV_ALIGN_CENTER, 0, -22);
    ui_label_set_fmt(self->pin_label, "%04d", self->pin);

    self->status_label = ui_label(scr, &lv_font_montserrat_14, UI_DIM_COLOR);
    lv_obj_align(self->status_label, LV_ALIGN_CENTER, 0, 18);

    self->file_label = ui_label(scr, &lv_font_montserrat_14, 0xffffff);
    lv_obj_set_width(self->file_label, 200);
    lv_label_set_long_mode(self->file_label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(self->file_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(self->file_label, LV_ALIGN_CENTER, 0, 40);

    self->bar = lv_bar_create(scr);
    lv_obj_set_size(self->bar, 180, 8);
    lv_obj_align(self->bar, LV_ALIGN_CENTER, 0, 62);
    lv_obj_set_style_bg_color(self->bar, lv_color_hex(0x1c2333), LV_PART_MAIN);
    lv_obj_set_style_bg_color(self->bar, lv_color_hex(SHARE_ACCENT_COLOR), LV_PART_INDICATOR);

    self->total_label = ui_label(scr, &lv_font_montserrat_14, UI_DIM_COLOR);
    lv_obj_align(self->total_label, LV_ALIGN_BOTTOM_MID, 0, -14);

    ui_status("Open the address, enter the PIN");
    self->online = wlan_is_online();
    listen_update(self->online);
    ui_update();
}

static void app_share_run(void *scr)
{
    int64_t now = k_uptime_get();
    bool online = wlan_is_online();

    if (online != self->online) {
        self->online = online;
        listen_update(online);
        ui_status(online ? "Open the address, enter the PIN" : "Waiting for Wi-Fi");
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

    if (self->ui_dirty && now - self->last_ui >= SHARE_UI_MS) {
        self->ui_dirty = false;
        self->last_ui = now;
        ui_update();
    }
}

static void app_share_exit(void *scr)
{
    conn_close();
    if (self->listen_fd >= 0) {
        zsock_close(self->listen_fd);
    }
}
