#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/net_event.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/shell/shell.h>
#include <string.h>
#include "net_job.h"
#include "sdconf.h"
#include "wlan.h"

LOG_MODULE_REGISTER(wlan, CONFIG_CUBE_LOG_LEVEL);

#define KEY_SSID "wifi_ssid"
#define KEY_PSK  "wifi_psk"

#define WIFI_EVENTS (NET_EVENT_WIFI_CONNECT_RESULT | NET_EVENT_WIFI_DISCONNECT_RESULT | \
                     NET_EVENT_WIFI_SCAN_RESULT | NET_EVENT_WIFI_SCAN_DONE)

// 斷線或連不上之後多久重試
#define RETRY_DELAY K_SECONDS(10)

// 連線要求會讀 SD 卡、等驅動啟動，換網路時還要等舊的連線斷開，所以在 net_job 的
// work queue 上跑，不卡住 system work queue（Timer app 在用）也不卡住 shell
static void connect_work_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(connect_work, connect_work_fn);

static struct net_mgmt_event_callback wifi_cb;
static struct net_mgmt_event_callback l4_cb;
static bool online;
// 重連由這裡負責，不用驅動的 ESP32_WIFI_STA_RECONNECT：驅動的重試和主動斷線會互相蓋掉，
// 斷線之後還會自己連回去。wanted 是使用者要不要連線，force 是要不要先斷開目前的連線
static bool wanted;
static bool force;
// 連不上時每 10 秒重試一次，只在第一次失敗記 log
static bool fail_logged;
// 掃描結果：每個 SSID 留最強的一筆，依 RSSI 排序。net_mgmt thread 寫入，app / shell 讀取。
// __noinit 放在 DRAM region 1，開始掃描時才清空
static wlan_ap_t scan_list[WLAN_SCAN_MAX] __noinit;
static int scan_count;
static bool scan_running;
// 驅動萬一沒送 SCAN_DONE，過了這個時間也當成掃完，不然之後都是 -EBUSY
#define SCAN_TIMEOUT_MS 10000
static int64_t scan_start_ms;
static struct k_spinlock scan_lock;

BUILD_ASSERT(WLAN_SSID_LEN == WIFI_SSID_MAX_LEN);

static struct net_if *wifi_iface(void)
{
    return net_if_get_wifi_sta();
}

static int iface_state(struct net_if *iface)
{
    struct wifi_iface_status status = { 0 };

    if (net_mgmt(NET_REQUEST_WIFI_IFACE_STATUS, iface, &status, sizeof(status)) < 0) {
        return WIFI_STATE_UNKNOWN;
    }
    return status.state;
}

// 插入一筆掃描結果，保持依 RSSI 由強到弱；同一個 SSID 只留最強的
static void scan_add(const struct wifi_scan_result *r)
{
    if (r->ssid_length == 0 || r->ssid_length > WLAN_SSID_LEN) {
        return;     // 隱藏網路
    }
    k_spinlock_key_t key = k_spin_lock(&scan_lock);
    int i;
    for (i = 0; i < scan_count; i++) {
        if (strlen(scan_list[i].ssid) == r->ssid_length &&
            memcmp(scan_list[i].ssid, r->ssid, r->ssid_length) == 0) {
            break;
        }
    }
    if (i < scan_count) {
        if (r->rssi <= scan_list[i].rssi) {
            k_spin_unlock(&scan_lock, key);
            return;
        }
        // 移除舊的那筆，下面重新插入
        memmove(&scan_list[i], &scan_list[i + 1], (scan_count - i - 1) * sizeof(wlan_ap_t));
        scan_count--;
    }
    int pos = 0;
    while (pos < scan_count && scan_list[pos].rssi >= r->rssi) {
        pos++;
    }
    if (pos < WLAN_SCAN_MAX) {
        int move = MIN(scan_count, WLAN_SCAN_MAX - 1) - pos;
        memmove(&scan_list[pos + 1], &scan_list[pos], move * sizeof(wlan_ap_t));
        wlan_ap_t *ap = &scan_list[pos];
        memcpy(ap->ssid, r->ssid, r->ssid_length);
        ap->ssid[r->ssid_length] = '\0';
        ap->rssi = r->rssi;
        ap->channel = r->channel;
        ap->security = r->security;
        scan_count = MIN(scan_count + 1, WLAN_SCAN_MAX);
    }
    k_spin_unlock(&scan_lock, key);
}

static void wifi_event_handler(struct net_mgmt_event_callback *cb, uint64_t event,
                               struct net_if *iface)
{
    const struct wifi_status *status = cb->info;

    switch (event) {
    case NET_EVENT_WIFI_CONNECT_RESULT:
        if (status->status == 0) {
            fail_logged = false;
            LOG_INF("Wi-Fi connected, waiting for DHCP");
        }
        else if (wanted) {
            if (!fail_logged) {
                fail_logged = true;
                LOG_WRN("Wi-Fi connect failed: %s, retrying every 10 s",
                        wifi_conn_status_txt(status->conn_status));
            }
            k_work_schedule_for_queue(net_job_queue(), &connect_work, RETRY_DELAY);
        }
        break;
    case NET_EVENT_WIFI_DISCONNECT_RESULT:
        online = false;
        LOG_INF("Wi-Fi disconnected");
        if (wanted) {
            k_work_schedule_for_queue(net_job_queue(), &connect_work, RETRY_DELAY);
        }
        break;
    case NET_EVENT_WIFI_SCAN_RESULT:
        scan_add(cb->info);
        break;
    case NET_EVENT_WIFI_SCAN_DONE:
        scan_running = false;
        break;
    default:
        break;
    }
}

// connection manager：介面 up 而且有 IP 才算上線
static void l4_event_handler(struct net_mgmt_event_callback *cb, uint64_t event,
                             struct net_if *iface)
{
    if (event == NET_EVENT_L4_DISCONNECTED) {
        online = false;
        return;
    }
    char addr[NET_IPV4_ADDR_LEN];
    const struct net_in_addr *ip = net_if_ipv4_get_global_addr(iface, NET_ADDR_PREFERRED);

    online = true;
    if (ip != NULL && net_addr_ntop(NET_AF_INET, ip, addr, sizeof(addr)) != NULL) {
        LOG_INF("Wi-Fi online, IP %s", addr);
    }
}

// 已連上或正在連
static bool link_busy(struct net_if *iface)
{
    int state = iface_state(iface);
    return state >= WIFI_STATE_SCANNING && state <= WIFI_STATE_COMPLETED;
}

// 驅動在連線中會拒絕新的連線要求 (-EALREADY)，先斷開，等狀態真的變了。
// 狀態比事件可靠：斷開正在進行的連線時，驅動送的是連線失敗而不是斷線事件
static void link_down(struct net_if *iface)
{
    if (!link_busy(iface)) {
        return;
    }
    (void)net_mgmt(NET_REQUEST_WIFI_DISCONNECT, iface, NULL, 0);
    for (int i = 0; i < 50 && link_busy(iface); i++) {
        k_msleep(100);
    }
}

static void connect_work_fn(struct k_work *work)
{
    char ssid[WIFI_SSID_MAX_LEN + 1];
    char psk[WIFI_PSK_MAX_LEN + 1];
    struct net_if *iface = wifi_iface();

    if (iface == NULL) {
        LOG_ERR("No Wi-Fi interface");
        return;
    }
    // 排程的重試：已經連上或正在連就不用動
    bool forced = force;
    force = false;
    if (!wanted || (!forced && link_busy(iface))) {
        return;
    }
    int ssid_len = sdconf_get(KEY_SSID, ssid, sizeof(ssid));
    if (ssid_len <= 0) {
        LOG_INF("No %s in %s, Wi-Fi stays off (see: wlan set)", KEY_SSID, SDCONF_PATH);
        return;
    }
    int psk_len = sdconf_get(KEY_PSK, psk, sizeof(psk));
    if (psk_len < 0) {
        psk_len = 0;
    }
    if (psk_len > 0 && psk_len < WIFI_PSK_MIN_LEN) {
        LOG_ERR("%s must be at least %d characters", KEY_PSK, WIFI_PSK_MIN_LEN);
        return;
    }

    struct wifi_connect_req_params params = {
        .ssid = (const uint8_t *)ssid,
        .ssid_length = ssid_len,
        .psk = (const uint8_t *)psk,
        .psk_length = psk_len,
        .security = psk_len > 0 ? WIFI_SECURITY_TYPE_PSK : WIFI_SECURITY_TYPE_NONE,
        .channel = WIFI_CHANNEL_ANY,
        .band = WIFI_FREQ_BAND_2_4_GHZ,
        .mfp = WIFI_MFP_OPTIONAL,
    };

    if (forced) {
        fail_logged = false;
        LOG_INF("Connecting to \"%s\"", ssid);
        link_down(iface);
    }
    int ret = net_mgmt(NET_REQUEST_WIFI_CONNECT, iface, &params, sizeof(params));
    if (ret < 0) {
        LOG_ERR("Connect request failed (%d)", ret);
        k_work_schedule_for_queue(net_job_queue(), &connect_work, RETRY_DELAY);
    }
    // 密碼不要留在 stack 上
    memset(psk, 0, sizeof(psk));
}

int wlan_connect(void)
{
    wanted = true;
    force = true;
    return k_work_reschedule_for_queue(net_job_queue(), &connect_work, K_NO_WAIT) < 0 ? -EIO : 0;
}

int wlan_disconnect(void)
{
    struct net_if *iface = wifi_iface();

    wanted = false;
    k_work_cancel_delayable(&connect_work);
    if (iface == NULL) {
        return -ENODEV;
    }
    return link_busy(iface) ? net_mgmt(NET_REQUEST_WIFI_DISCONNECT, iface, NULL, 0) : 0;
}

bool wlan_is_online(void)
{
    return online;
}

static void addr_text(const struct net_in_addr *addr, char *buf)
{
    if (addr == NULL || addr->s_addr == 0 ||
        net_addr_ntop(NET_AF_INET, addr, buf, WLAN_ADDR_LEN) == NULL) {
        buf[0] = '\0';
    }
}

int wlan_info(wlan_info_t *info)
{
    struct net_if *iface = wifi_iface();
    struct wifi_iface_status status = { 0 };

    memset(info, 0, sizeof(*info));
    info->state = WIFI_STATE_UNKNOWN;
    if (iface == NULL) {
        return -ENODEV;
    }

    struct net_linkaddr *mac = net_if_get_link_addr(iface);
    if (mac != NULL && mac->len == sizeof(info->mac)) {
        memcpy(info->mac, mac->addr, sizeof(info->mac));
    }
    if (net_mgmt(NET_REQUEST_WIFI_IFACE_STATUS, iface, &status, sizeof(status)) == 0) {
        info->state = status.state;
        if (status.state == WIFI_STATE_COMPLETED) {
            memcpy(info->ssid, status.ssid, MIN(status.ssid_len, WLAN_SSID_LEN));
            info->rssi = status.rssi;
            info->channel = status.channel;
            info->security = status.security;
        }
    }

    info->online = online;
    const struct net_in_addr *ip = net_if_ipv4_get_global_addr(iface, NET_ADDR_PREFERRED);
    if (ip != NULL) {
        struct net_in_addr mask = net_if_ipv4_get_netmask_by_addr(iface, ip);
        struct net_in_addr gw = net_if_ipv4_get_gw(iface);
        addr_text(ip, info->ip);
        addr_text(&mask, info->netmask);
        addr_text(&gw, info->gateway);
    }
    return 0;
}

int wlan_scan(void)
{
    struct net_if *iface = wifi_iface();

    if (iface == NULL) {
        return -ENODEV;
    }
    if (wlan_scan_busy()) {
        return -EBUSY;
    }
    k_spinlock_key_t key = k_spin_lock(&scan_lock);
    scan_count = 0;
    scan_running = true;
    scan_start_ms = k_uptime_get();
    k_spin_unlock(&scan_lock, key);

    int ret = net_mgmt(NET_REQUEST_WIFI_SCAN, iface, NULL, 0);
    if (ret < 0) {
        scan_running = false;
    }
    return ret;
}

bool wlan_scan_busy(void)
{
    if (scan_running && k_uptime_get() - scan_start_ms > SCAN_TIMEOUT_MS) {
        scan_running = false;
    }
    return scan_running;
}

int wlan_scan_get(wlan_ap_t *out, int max)
{
    k_spinlock_key_t key = k_spin_lock(&scan_lock);
    int n = MIN(max, scan_count);
    memcpy(out, scan_list, n * sizeof(wlan_ap_t));
    k_spin_unlock(&scan_lock, key);
    return n;
}

const char *wlan_security_txt(int security)
{
    return wifi_security_txt(security);
}

static int wlan_init(void)
{
    net_mgmt_init_event_callback(&wifi_cb, wifi_event_handler, WIFI_EVENTS);
    net_mgmt_add_event_callback(&wifi_cb);
    net_mgmt_init_event_callback(&l4_cb, l4_event_handler,
                                 NET_EVENT_L4_CONNECTED | NET_EVENT_L4_DISCONNECTED);
    net_mgmt_add_event_callback(&l4_cb);

    // SD 卡在這之前就掛載好了 (FILE_INIT_PRIORITY)
    return wlan_connect();
}

SYS_INIT(wlan_init, APPLICATION, CONFIG_WLAN_INIT_PRIORITY);

/* ---------- shell ---------- */

static int _wlan_status(const struct shell *shell, size_t argc, char **argv)
{
    char ssid[WIFI_SSID_MAX_LEN + 1];
    wlan_info_t info;

    if (sdconf_get(KEY_SSID, ssid, sizeof(ssid)) <= 0) {
        strcpy(ssid, "(not set)");
    }
    // PSK 只顯示有沒有設定
    char psk[WIFI_PSK_MAX_LEN + 1];
    int psk_len = sdconf_get(KEY_PSK, psk, sizeof(psk));
    memset(psk, 0, sizeof(psk));
    shell_print(shell, "Config: %s  ssid \"%s\"  psk %s", SDCONF_PATH, ssid,
                psk_len > 0 ? "set" : "none (open network)");

    if (wlan_info(&info) < 0) {
        shell_print(shell, "No Wi-Fi interface");
        return -ENODEV;
    }
    shell_print(shell, "State: %s", wifi_state_txt(info.state));
    if (info.state == WIFI_STATE_COMPLETED) {
        shell_print(shell, "Link: \"%s\"  ch %u  %d dBm  %s", info.ssid, info.channel, info.rssi,
                    wifi_security_txt(info.security));
    }
    if (info.ip[0] != '\0') {
        shell_print(shell, "IP: %s  netmask %s  gateway %s", info.ip, info.netmask, info.gateway);
    }
    else {
        shell_print(shell, "IP: none");
    }
    shell_print(shell, "MAC: %02x:%02x:%02x:%02x:%02x:%02x", info.mac[0], info.mac[1], info.mac[2],
                info.mac[3], info.mac[4], info.mac[5]);
    return 0;
}

static int _wlan_set(const struct shell *shell, size_t argc, char **argv)
{
    const char *ssid = argv[1];
    const char *psk = argc > 2 ? argv[2] : NULL;

    if (strlen(ssid) == 0 || strlen(ssid) > WIFI_SSID_MAX_LEN) {
        shell_error(shell, "SSID must be 1 to %d characters", WIFI_SSID_MAX_LEN);
        return -EINVAL;
    }
    if (psk != NULL && (strlen(psk) < WIFI_PSK_MIN_LEN || strlen(psk) > WIFI_PSK_MAX_LEN)) {
        shell_error(shell, "PSK must be %d to %d characters", WIFI_PSK_MIN_LEN, WIFI_PSK_MAX_LEN);
        return -EINVAL;
    }

    int ret = sdconf_set(KEY_SSID, ssid);
    ret = ret ?: sdconf_set(KEY_PSK, psk);
    if (ret < 0) {
        shell_error(shell, "Failed to write %s (%d), is the SD card in?", SDCONF_PATH, ret);
        return ret;
    }
    shell_print(shell, "Saved to %s, connecting to \"%s\"", SDCONF_PATH, ssid);
    return wlan_connect();
}

static int _wlan_forget(const struct shell *shell, size_t argc, char **argv)
{
    int ret = sdconf_set(KEY_SSID, NULL);
    ret = ret ?: sdconf_set(KEY_PSK, NULL);
    if (ret < 0) {
        shell_error(shell, "Failed to write %s (%d)", SDCONF_PATH, ret);
        return ret;
    }
    shell_print(shell, "Removed the Wi-Fi settings from %s", SDCONF_PATH);
    (void)wlan_disconnect();
    return 0;
}

static int _wlan_connect(const struct shell *shell, size_t argc, char **argv)
{
    return wlan_connect();
}

static int _wlan_disconnect(const struct shell *shell, size_t argc, char **argv)
{
    int ret = wlan_disconnect();
    if (ret < 0) {
        shell_error(shell, "Disconnect failed (%d)", ret);
    }
    return ret;
}

static int _wlan_scan(const struct shell *shell, size_t argc, char **argv)
{
    int ret = wlan_scan();
    if (ret < 0) {
        shell_error(shell, "Scan failed (%d)", ret);
        return ret;
    }
    // 掃描大約 2 到 4 秒，等它做完再一次印出排序好的結果
    for (int i = 0; i < 100 && wlan_scan_busy(); i++) {
        k_msleep(100);
    }
    wlan_ap_t aps[WLAN_SCAN_MAX];
    int n = wlan_scan_get(aps, ARRAY_SIZE(aps));
    for (int i = 0; i < n; i++) {
        shell_print(shell, "%-32s  ch %3u  %4d dBm  %s", aps[i].ssid, aps[i].channel, aps[i].rssi,
                    wifi_security_txt(aps[i].security));
    }
    // shell_print 靠字串拼接加換行，格式不能是運算式
    if (n == WLAN_SCAN_MAX) {
        shell_print(shell, "%d networks (only the strongest are kept)", n);
    }
    else {
        shell_print(shell, "%d networks", n);
    }
    return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_wlan,
    SHELL_CMD_ARG(status, NULL, "Config, link state and IP address", _wlan_status, 1, 0),
    SHELL_CMD_ARG(set, NULL,
                  "Save to the SD card config and connect: wlan set <ssid> [psk]\n"
                  "Quote an SSID with spaces: wlan set \"My Network\" password",
                  _wlan_set, 2, 1),
    SHELL_CMD_ARG(forget, NULL, "Remove the SSID / PSK from the SD card config and disconnect",
                  _wlan_forget, 1, 0),
    SHELL_CMD_ARG(connect, NULL, "Connect again with the saved config", _wlan_connect, 1, 0),
    SHELL_CMD_ARG(disconnect, NULL, "Disconnect, no reconnect until wlan connect",
                  _wlan_disconnect, 1, 0),
    SHELL_CMD_ARG(scan, NULL, "List nearby networks", _wlan_scan, 1, 0),
    SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(wlan, &sub_wlan, "Wi-Fi commands", NULL);
