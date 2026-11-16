#include <zephyr/kernel.h>
#include <zephyr/fs/fs.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <ctype.h>
#include <string.h>
#include <strings.h>
#include "file.h"
#include "sdconf.h"

LOG_MODULE_REGISTER(sdconf, CONFIG_GUI_LOG_LEVEL);

// shell 和 Wi-Fi 的 work queue 都會讀寫，兩塊 buffer 共用一把鎖
static K_MUTEX_DEFINE(sdconf_mutex);
// __noinit 放在 DRAM region 1，region 0 已經被 Wi-Fi 用滿；每次都會先讀檔再用
static char file_buf[SDCONF_MAX_SIZE + 1] __noinit;
static char out_buf[SDCONF_MAX_SIZE + 1] __noinit;

// 讀整個檔案到 file_buf，回傳長度；檔案不存在當成空檔
static int load_file(void)
{
    struct fs_file_t file;

    if (!file_exists(SDCONF_PATH)) {
        file_buf[0] = '\0';
        return 0;
    }
    fs_file_t_init(&file);
    int ret = fs_open(&file, SDCONF_PATH, FS_O_READ);
    if (ret < 0) {
        return ret;
    }
    // 多讀 1 byte，讀得到就代表檔案超過上限
    char extra;
    ssize_t len = fs_read(&file, file_buf, SDCONF_MAX_SIZE);
    ssize_t more = len == SDCONF_MAX_SIZE ? fs_read(&file, &extra, 1) : 0;
    fs_close(&file);
    if (len < 0) {
        return len;
    }
    if (more > 0) {
        LOG_ERR("%s is larger than %d bytes", SDCONF_PATH, SDCONF_MAX_SIZE);
        return -EFBIG;
    }
    file_buf[len] = '\0';
    return len;
}

// 去掉頭尾空白，回傳開頭並把長度寫進 len
static const char *trim(const char *s, size_t *len)
{
    while (*len > 0 && isspace((unsigned char)s[0])) {
        s++;
        (*len)--;
    }
    while (*len > 0 && isspace((unsigned char)s[*len - 1])) {
        (*len)--;
    }
    return s;
}

// line 是不是 "key=..."，是的話把 value 的位置和長度寫出來
static bool match_key(const char *line, size_t line_len, const char *key, const char **value,
                      size_t *value_len)
{
    const char *eq = memchr(line, '=', line_len);
    if (eq == NULL || line[0] == '#') {
        return false;
    }
    size_t key_len = eq - line;
    const char *k = trim(line, &key_len);
    if (key_len != strlen(key) || strncmp(k, key, key_len) != 0) {
        return false;
    }
    *value_len = line_len - (eq + 1 - line);
    *value = trim(eq + 1, value_len);
    return true;
}

// 這一行（不含換行）的長度，*next 指到下一行開頭；\r\n、\n、\r 都算一個換行，空白行也保留
static size_t line_end(const char *line, const char **next)
{
    size_t len = strcspn(line, "\r\n");
    const char *p = line + len;
    if (p[0] == '\r' && p[1] == '\n') {
        p += 2;
    }
    else if (p[0] != '\0') {
        p++;
    }
    *next = p;
    return len;
}

int sdconf_get(const char *key, char *buf, size_t len)
{
    k_mutex_lock(&sdconf_mutex, K_FOREVER);
    int ret = load_file();
    if (ret >= 0) {
        ret = -ENOENT;
        for (const char *line = file_buf, *next; *line != '\0'; line = next) {
            size_t line_len = line_end(line, &next);
            const char *value;
            size_t value_len;
            if (match_key(line, line_len, key, &value, &value_len)) {
                if (value_len + 1 > len) {
                    ret = -ENOSPC;
                }
                else {
                    memcpy(buf, value, value_len);
                    buf[value_len] = '\0';
                    ret = value_len;
                }
                break;
            }
        }
    }
    k_mutex_unlock(&sdconf_mutex);
    return ret;
}

static int append(size_t *pos, const char *s, size_t n)
{
    if (*pos + n > SDCONF_MAX_SIZE) {
        return -EFBIG;
    }
    memcpy(out_buf + *pos, s, n);
    *pos += n;
    return 0;
}

static int write_file(size_t len)
{
    struct fs_file_t file;

    fs_file_t_init(&file);
    int ret = fs_open(&file, SDCONF_PATH, FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC);
    if (ret < 0) {
        return ret;
    }
    ssize_t written = fs_write(&file, out_buf, len);
    ret = fs_close(&file);
    if (written < 0) {
        return written;
    }
    return written != len ? -EIO : ret;
}

int sdconf_set(const char *key, const char *value)
{
    if (strpbrk(key, "=\r\n#") != NULL || (value != NULL && strpbrk(value, "\r\n") != NULL)) {
        return -EINVAL;
    }

    k_mutex_lock(&sdconf_mutex, K_FOREVER);
    int ret = load_file();
    size_t pos = 0;
    bool found = false;

    if (ret >= 0) {
        ret = 0;
        for (const char *line = file_buf, *next; *line != '\0'; line = next) {
            size_t line_len = line_end(line, &next);
            const char *old;
            size_t old_len;
            bool is_key = match_key(line, line_len, key, &old, &old_len);
            if (is_key && (found || value == NULL)) {
                continue;   // 刪掉，或同一個 key 出現第二次
            }
            if (is_key) {
                found = true;
                ret = append(&pos, key, strlen(key));
                ret = ret ?: append(&pos, "=", 1);
                ret = ret ?: append(&pos, value, strlen(value));
            }
            else {
                ret = append(&pos, line, line_len);
            }
            ret = ret ?: append(&pos, "\n", 1);
            if (ret) {
                break;
            }
        }
        if (ret == 0 && !found && value != NULL) {
            ret = append(&pos, key, strlen(key));
            ret = ret ?: append(&pos, "=", 1);
            ret = ret ?: append(&pos, value, strlen(value));
            ret = ret ?: append(&pos, "\n", 1);
        }
        if (ret == 0) {
            ret = write_file(pos);
        }
    }
    k_mutex_unlock(&sdconf_mutex);

    if (ret < 0) {
        LOG_ERR("Failed to write %s (%d)", SDCONF_PATH, ret);
    }
    return ret;
}

/* ---------- shell ---------- */

// 值裡有密碼的 key 不印出來
static bool is_secret(const char *key, size_t len)
{
    for (size_t i = 0; i + 3 <= len; i++) {
        if (strncasecmp(key + i, "psk", 3) == 0 || strncasecmp(key + i, "pas", 3) == 0) {
            return true;
        }
    }
    return false;
}

static int _config_list(const struct shell *shell, size_t argc, char **argv)
{
    k_mutex_lock(&sdconf_mutex, K_FOREVER);
    int ret = load_file();
    if (ret == 0) {
        shell_print(shell, "%s is empty or missing", SDCONF_PATH);
    }
    for (const char *line = file_buf, *next; ret > 0 && *line != '\0'; line = next) {
        size_t line_len = line_end(line, &next);
        const char *eq = memchr(line, '=', line_len);
        if (eq != NULL && line[0] != '#' && is_secret(line, eq - line)) {
            shell_print(shell, "%.*s= ********", (int)(eq - line), line);
        }
        else {
            shell_print(shell, "%.*s", (int)line_len, line);
        }
    }
    k_mutex_unlock(&sdconf_mutex);
    if (ret < 0) {
        shell_error(shell, "Failed to read %s (%d)", SDCONF_PATH, ret);
    }
    return ret < 0 ? ret : 0;
}

static int _config_get(const struct shell *shell, size_t argc, char **argv)
{
    char value[128];
    int ret = sdconf_get(argv[1], value, sizeof(value));

    if (ret == -ENOENT) {
        shell_print(shell, "%s is not set", argv[1]);
        return 0;
    }
    if (ret < 0) {
        shell_error(shell, "Failed to read %s (%d)", argv[1], ret);
        return ret;
    }
    shell_print(shell, "%s=%s", argv[1], is_secret(argv[1], strlen(argv[1])) ? "********" : value);
    return 0;
}

static int _config_set(const struct shell *shell, size_t argc, char **argv)
{
    int ret = sdconf_set(argv[1], argv[2]);
    if (ret < 0) {
        shell_error(shell, "Failed to write %s (%d)", SDCONF_PATH, ret);
        return ret;
    }
    shell_print(shell, "%s=%s", argv[1], is_secret(argv[1], strlen(argv[1])) ? "********" : argv[2]);
    return 0;
}

static int _config_unset(const struct shell *shell, size_t argc, char **argv)
{
    int ret = sdconf_set(argv[1], NULL);
    if (ret < 0) {
        shell_error(shell, "Failed to write %s (%d)", SDCONF_PATH, ret);
        return ret;
    }
    shell_print(shell, "Removed %s", argv[1]);
    return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_config,
    SHELL_CMD_ARG(list, NULL, "Show " SDCONF_PATH " (passwords hidden)", _config_list, 1, 0),
    SHELL_CMD_ARG(get, NULL, "Show one value: config get <key>", _config_get, 2, 0),
    SHELL_CMD_ARG(set, NULL, "Set a value: config set <key> <value> (quote values with spaces)",
                  _config_set, 3, 0),
    SHELL_CMD_ARG(unset, NULL, "Remove a key: config unset <key>", _config_unset, 2, 0),
    SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(config, &sub_config, "SD card config (" SDCONF_PATH ")", NULL);
