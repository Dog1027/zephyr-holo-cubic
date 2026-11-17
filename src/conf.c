#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/wifi.h>
#include <zephyr/shell/shell.h>
#include <string.h>
#include "conf.h"
#include "net_job.h"
#include "net_time.h"
#include "prefs.h"
#include "sdconf.h"
#include "wlan.h"

LOG_MODULE_REGISTER(conf, CONFIG_CUBE_LOG_LEVEL);

// 值改了之後要通知誰
#define APPLY_WIFI BIT(0)
#define APPLY_TZ   BIT(1)

typedef struct {
    const char *key;        // 使用者看到的名稱，也是 /config.txt 裡的 key
    const char *pref;       // settings 裡的名稱
    uint8_t max_len;
    uint8_t apply;
    bool secret;            // 不印出來
    bool (*valid)(const char *value);
    const char *unset_text; // 沒設定時 config list 顯示的說明
} conf_item_t;

static bool psk_valid(const char *value)
{
    return strlen(value) >= WIFI_PSK_MIN_LEN;
}

// 新增設定：在這裡加一行，conf.h 加一個名稱，用到的地方 conf_get()
static const conf_item_t items[] = {
    {CONF_WIFI_SSID, "wifi/ssid", WIFI_SSID_MAX_LEN, APPLY_WIFI, false, NULL, "Wi-Fi stays off"},
    {CONF_WIFI_PSK, "wifi/psk", WIFI_PSK_MAX_LEN, APPLY_WIFI, true, psk_valid, "open network"},
    {CONF_TZ, "time/tz", 6, APPLY_TZ, false, net_time_tz_valid, "+8"},
    {CONF_NTP_SERVER, "time/server", 63, 0, false, NULL, "pool.ntp.org"},
    {CONF_WEATHER_LOCATION, "weather/loc", 47, 0, false, NULL, "from the IP address"},
};

BUILD_ASSERT(WIFI_PSK_MAX_LEN <= CONF_VALUE_MAX);

static void import_work_fn(struct k_work *work);
static K_WORK_DEFINE(import_work, import_work_fn);

static const conf_item_t *item_find(const char *key)
{
    ARRAY_FOR_EACH_PTR(items, item) {
        if (strcmp(item->key, key) == 0) {
            return item;
        }
    }
    return NULL;
}

// settings 裡存的是含結尾 NUL 的字串，沒設定過或清掉了都是空字串
static int item_get(const conf_item_t *item, char *buf, size_t len)
{
    int n = prefs_load(item->pref, buf, len - 1);

    buf[MAX(n, 0)] = '\0';
    return strlen(buf);
}

static int item_set(const conf_item_t *item, const char *value)
{
    char old[CONF_VALUE_MAX + 1];

    if (value == NULL) {
        value = "";
    }
    if (strlen(value) > item->max_len || (value[0] != '\0' && item->valid != NULL && !item->valid(value))) {
        return -EINVAL;
    }
    // 內容一樣就不寫 flash
    item_get(item, old, sizeof(old));
    int ret = strcmp(old, value) == 0 ? 0 : prefs_save(item->pref, value, strlen(value) + 1);
    bool changed = ret == 0 && strcmp(old, value) != 0;

    memset(old, 0, sizeof(old));
    return ret < 0 ? ret : changed;
}

int conf_get(const char *key, char *buf, size_t len)
{
    const conf_item_t *item = item_find(key);

    if (item == NULL) {
        buf[0] = '\0';
        return -ENOENT;
    }
    return item_get(item, buf, len);
}

int conf_set(const char *key, const char *value)
{
    const conf_item_t *item = item_find(key);

    return item ? item_set(item, value) : -ENOENT;
}

static void apply(uint8_t what)
{
    if (what & APPLY_TZ) {
        net_time_load_tz();
    }
    if (what & APPLY_WIFI) {
        (void)wlan_connect();
    }
}

// 在 net_job 的 work queue 上跑：會讀寫 SD 卡和 flash，stack 也比開機的 main 大
static void import_work_fn(struct k_work *work)
{
    char value[CONF_VALUE_MAX + 1];
    uint8_t changed = 0;
    bool ssid_seen = false;
    bool psk_seen = false;

    ARRAY_FOR_EACH_PTR(items, item) {
        int len = sdconf_get(item->key, value, item->max_len + 1);
        if (len == -ENOENT) {
            continue;   // 沒有這一行、沒有檔案或沒插卡
        }
        int ret = len < 0 ? -EINVAL : item_set(item, value);
        if (ret < 0) {
            LOG_WRN("Bad %s in %s (%d), not imported", item->key, SDCONF_PATH, ret);
            continue;   // 留在檔案裡讓使用者看得到
        }
        LOG_INF("Imported %s from %s%s", item->key, SDCONF_PATH, ret ? "" : " (same as saved)");
        if (ret > 0) {
            changed |= item->apply;
        }
        ssid_seen |= strcmp(item->key, CONF_WIFI_SSID) == 0;
        psk_seen |= strcmp(item->key, CONF_WIFI_PSK) == 0;
        // 存好了就從檔案刪掉：設定只留在一個地方，密碼也不留在卡上
        if (sdconf_set(item->key, NULL) < 0) {
            LOG_WRN("Cannot remove %s from %s, it will be imported again", item->key, SDCONF_PATH);
        }
    }
    memset(value, 0, sizeof(value));

    // 只給了 SSID 沒給密碼：是開放網路，不要留著上一個網路的密碼
    if (ssid_seen && !psk_seen && conf_set(CONF_WIFI_PSK, NULL) > 0) {
        changed |= APPLY_WIFI;
    }
    apply(changed);
}

void conf_import(void)
{
    k_work_submit_to_queue(net_job_queue(), &import_work);
}

// 排在 SD 卡掛載之後、wlan / net_time 之前：匯入的工作會排在第一次 Wi-Fi 連線的前面
static int conf_init(void)
{
    char value[CONF_VALUE_MAX + 1];

    // settings 第一次用到時才初始化，先在這裡做完，之後各個 thread 才不會同時去初始化
    (void)item_get(&items[0], value, sizeof(value));
    conf_import();
    return 0;
}

SYS_INIT(conf_init, APPLICATION, CONFIG_CONF_INIT_PRIORITY);

/* ---------- shell ---------- */

static const conf_item_t *shell_item(const struct shell *shell, const char *key)
{
    const conf_item_t *item = item_find(key);

    if (item == NULL) {
        shell_error(shell, "Unknown key %s, see: config list", key);
    }
    return item;
}

static void item_print(const struct shell *shell, const conf_item_t *item)
{
    char value[CONF_VALUE_MAX + 1];

    if (item_get(item, value, sizeof(value)) == 0) {
        shell_print(shell, "%-18s (not set: %s)", item->key, item->unset_text);
    }
    else {
        shell_print(shell, "%-18s %s", item->key, item->secret ? "********" : value);
    }
    memset(value, 0, sizeof(value));
}

static int _config_list(const struct shell *shell, size_t argc, char **argv)
{
    ARRAY_FOR_EACH_PTR(items, item) {
        item_print(shell, item);
    }
    return 0;
}

static int _config_get(const struct shell *shell, size_t argc, char **argv)
{
    const conf_item_t *item = shell_item(shell, argv[1]);

    if (item == NULL) {
        return -ENOENT;
    }
    item_print(shell, item);
    return 0;
}

static int config_change(const struct shell *shell, const char *key, const char *value)
{
    const conf_item_t *item = shell_item(shell, key);

    if (item == NULL) {
        return -ENOENT;
    }
    int ret = item_set(item, value);
    if (ret == -EINVAL) {
        shell_error(shell, "%s does not accept that value (wrong format, or over %d characters)", key, item->max_len);
        return ret;
    }
    if (ret < 0) {
        shell_error(shell, "Failed to save %s (%d)", key, ret);
        return ret;
    }
    item_print(shell, item);
    if (ret > 0) {
        apply(item->apply);
    }
    return 0;
}

static int _config_set(const struct shell *shell, size_t argc, char **argv)
{
    return config_change(shell, argv[1], argv[2]);
}

static int _config_unset(const struct shell *shell, size_t argc, char **argv)
{
    return config_change(shell, argv[1], NULL);
}

static int _config_import(const struct shell *shell, size_t argc, char **argv)
{
    conf_import();
    shell_print(shell, "Importing %s in the background, the result is logged", SDCONF_PATH);
    return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_config,
    SHELL_CMD_ARG(list, NULL, "Show all settings (passwords hidden)", _config_list, 1, 0),
    SHELL_CMD_ARG(get, NULL, "Show one value: config get <key>", _config_get, 2, 0),
    SHELL_CMD_ARG(set, NULL, "Set a value: config set <key> <value> (quote values with spaces)",
                  _config_set, 3, 0),
    SHELL_CMD_ARG(unset, NULL, "Back to the default: config unset <key>", _config_unset, 2, 0),
    SHELL_CMD_ARG(import, NULL, "Import " SDCONF_PATH " now (also done at boot)", _config_import, 1, 0),
    SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(config, &sub_config, "Settings (kept in flash)", NULL);
