#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/net_event.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/shell/shell.h>
#include <string.h>
#include "sdconf.h"
#include "wlan.h"

LOG_MODULE_REGISTER(wlan, CONFIG_WLAN_LOG_LEVEL);

#define KEY_SSID "wifi_ssid"
#define KEY_PSK  "wifi_psk"

#define WIFI_EVENTS (NET_EVENT_WIFI_CONNECT_RESULT | NET_EVENT_WIFI_DISCONNECT_RESULT | \
                     NET_EVENT_WIFI_SCAN_RESULT | NET_EVENT_WIFI_SCAN_DONE)

// 斷線或連不上之後多久重試
#define RETRY_DELAY K_SECONDS(10)

// 連線要求會讀 SD 卡、等驅動啟動，換網路時還要等舊的連線斷開，所以放在自己的 work queue，
// 不卡住 system work queue（Timer app 在用）也不卡住 shell。stack 是 noinit，在 DRAM region 1
static K_THREAD_STACK_DEFINE(wlan_wq_stack, CONFIG_WLAN_WORKQ_STACK_SIZE);
static struct k_work_q wlan_wq;
static void connect_work_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(connect_work, connect_work_fn);

static struct net_mgmt_event_callback wifi_cb;
static struct net_mgmt_event_callback ipv4_cb;
static bool online;
// 重連由這裡負責，不用驅動的 ESP32_WIFI_STA_RECONNECT：驅動的重試和主動斷線會互相蓋掉，
// 斷線之後還會自己連回去。wanted 是使用者要不要連線，force 是要不要先斷開目前的連線
static bool wanted;
static bool force;
// 連不上時每 10 秒重試一次，只在第一次失敗記 log
static bool fail_logged;
// wlan scan 的結果要印回下指令的 shell
static const struct shell *scan_shell;

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
            k_work_schedule_for_queue(&wlan_wq, &connect_work, RETRY_DELAY);
        }
        break;
    case NET_EVENT_WIFI_DISCONNECT_RESULT:
        online = false;
        LOG_INF("Wi-Fi disconnected");
        if (wanted) {
            k_work_schedule_for_queue(&wlan_wq, &connect_work, RETRY_DELAY);
        }
        break;
    case NET_EVENT_WIFI_SCAN_RESULT: {
        const struct wifi_scan_result *r = cb->info;
        if (scan_shell != NULL) {
            shell_print(scan_shell, "%-32.*s  ch %3u  %4d dBm  %s", r->ssid_length, r->ssid,
                        r->channel, r->rssi, wifi_security_txt(r->security));
        }
        break;
    }
    case NET_EVENT_WIFI_SCAN_DONE:
        if (scan_shell != NULL) {
            shell_print(scan_shell, "Scan done");
            scan_shell = NULL;
        }
        break;
    default:
        break;
    }
}

static void ipv4_event_handler(struct net_mgmt_event_callback *cb, uint64_t event,
                               struct net_if *iface)
{
    char addr[NET_IPV4_ADDR_LEN];
    const struct net_in_addr *ip = net_if_ipv4_get_global_addr(iface, NET_ADDR_PREFERRED);

    if (ip != NULL && net_addr_ntop(NET_AF_INET, ip, addr, sizeof(addr)) != NULL) {
        online = true;
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
        k_work_schedule_for_queue(&wlan_wq, &connect_work, RETRY_DELAY);
    }
    // 密碼不要留在 stack 上
    memset(psk, 0, sizeof(psk));
}

int wlan_connect(void)
{
    wanted = true;
    force = true;
    return k_work_reschedule_for_queue(&wlan_wq, &connect_work, K_NO_WAIT) < 0 ? -EIO : 0;
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

static int wlan_init(void)
{
    const struct k_work_queue_config cfg = { .name = "wlan_wq" };

    k_work_queue_start(&wlan_wq, wlan_wq_stack, K_THREAD_STACK_SIZEOF(wlan_wq_stack),
                       K_LOWEST_APPLICATION_THREAD_PRIO, &cfg);

    net_mgmt_init_event_callback(&wifi_cb, wifi_event_handler, WIFI_EVENTS);
    net_mgmt_add_event_callback(&wifi_cb);
    net_mgmt_init_event_callback(&ipv4_cb, ipv4_event_handler, NET_EVENT_IPV4_ADDR_ADD);
    net_mgmt_add_event_callback(&ipv4_cb);

    // SD 卡在這之前就掛載好了 (FILE_INIT_PRIORITY)
    return wlan_connect();
}

SYS_INIT(wlan_init, APPLICATION, CONFIG_WLAN_INIT_PRIORITY);

/* ---------- shell ---------- */

static int _wlan_status(const struct shell *shell, size_t argc, char **argv)
{
    char ssid[WIFI_SSID_MAX_LEN + 1];
    struct net_if *iface = wifi_iface();

    if (sdconf_get(KEY_SSID, ssid, sizeof(ssid)) <= 0) {
        strcpy(ssid, "(not set)");
    }
    // PSK 只顯示有沒有設定
    char psk[WIFI_PSK_MAX_LEN + 1];
    int psk_len = sdconf_get(KEY_PSK, psk, sizeof(psk));
    memset(psk, 0, sizeof(psk));
    shell_print(shell, "Config: %s  ssid \"%s\"  psk %s", SDCONF_PATH, ssid,
                psk_len > 0 ? "set" : "none (open network)");

    if (iface == NULL) {
        shell_print(shell, "No Wi-Fi interface");
        return -ENODEV;
    }

    struct wifi_iface_status status = { 0 };
    if (net_mgmt(NET_REQUEST_WIFI_IFACE_STATUS, iface, &status, sizeof(status)) < 0) {
        shell_print(shell, "State: unknown");
        return 0;
    }
    shell_print(shell, "State: %s", wifi_state_txt(status.state));
    if (status.state == WIFI_STATE_COMPLETED) {
        shell_print(shell, "Link: \"%.*s\"  ch %u  %d dBm  %s", status.ssid_len, status.ssid,
                    status.channel, status.rssi, wifi_security_txt(status.security));
    }

    char addr[NET_IPV4_ADDR_LEN];
    const struct net_in_addr *ip = net_if_ipv4_get_global_addr(iface, NET_ADDR_PREFERRED);
    if (ip != NULL && net_addr_ntop(NET_AF_INET, ip, addr, sizeof(addr)) != NULL) {
        shell_print(shell, "IP: %s", addr);
    }
    else {
        shell_print(shell, "IP: none");
    }
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
    struct net_if *iface = wifi_iface();

    if (iface == NULL) {
        shell_error(shell, "No Wi-Fi interface");
        return -ENODEV;
    }
    scan_shell = shell;
    int ret = net_mgmt(NET_REQUEST_WIFI_SCAN, iface, NULL, 0);
    if (ret < 0) {
        scan_shell = NULL;
        shell_error(shell, "Scan failed (%d)", ret);
    }
    return ret;
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
