#pragma once

#include <lvgl.h>
#include <zephyr/fs/fs.h>
#include <stdbool.h>
#include <stdint.h>

/*
 * Files app 的 SHARE 分頁：網頁版的檔案管理（HTTP server，細節在 share_page.c 開頭）。
 * 狀態放在 Files 的 self_t 裡，不用初始化。只有這一頁顯示時才提供服務：
 * 切到這頁時 share_page_start()，離開或關 app 時 share_page_stop()，顯示時每次 run 呼叫 share_page_run()。
 */

#define SHARE_PATH_MAX      96
#define SHARE_LINE_MAX      160     // 更長的 header 只看開頭，用不到的部分就丟掉
#define SHARE_NAME_MAX      100     // 列表裡更長的檔名不列出來

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
} share_page_t;

// 在 scr（整個畫面大小的容器）裡建立畫面，內容在分頁籤以下
void share_page_create(share_page_t *state, lv_obj_t *scr);

// 開始聽 port 80，log 印出 PIN（自動測試靠這行拿 PIN）
void share_page_start(void);

void share_page_run(void);

// 關掉連線和 server
void share_page_stop(void);

// 換一組 PIN，舊的馬上失效
void share_page_new_pin(void);
