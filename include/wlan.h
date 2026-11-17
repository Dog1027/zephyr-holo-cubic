#pragma once

#include <stdbool.h>
#include <stdint.h>

/*
 * Wi-Fi station. The SSID and PSK are kept in the flash settings (prefs.h), so it connects
 * without an SD card. The SD card config (sdconf.h) is a way to put them there: when it has
 *   wifi_ssid=MyNetwork
 *   wifi_psk=password      (leave it out for an open network)
 * they are copied over the saved ones every time it connects; without these lines (or without
 * the file or the card) the saved ones are used as they are.
 * It connects at boot and retries every 10 s after the link drops.
 * Shell: wlan set / status / forget / connect / disconnect / scan
 */

#define WLAN_SSID_LEN 32
#define WLAN_ADDR_LEN 16    // "255.255.255.255" + NUL
#define WLAN_SCAN_MAX 12

typedef struct {
    int state;                      // enum wifi_iface_state, WIFI_STATE_COMPLETED when linked
    bool online;                    // linked and DHCP gave an address
    char ssid[WLAN_SSID_LEN + 1];   // linked network, empty when not linked
    int rssi;                       // dBm
    unsigned int channel;
    int security;                   // enum wifi_security_type
    char ip[WLAN_ADDR_LEN];         // empty without an address
    char netmask[WLAN_ADDR_LEN];
    char gateway[WLAN_ADDR_LEN];
    uint8_t mac[6];
} wlan_info_t;

typedef struct {
    char ssid[WLAN_SSID_LEN + 1];
    int8_t rssi;
    uint8_t channel;
    uint8_t security;               // enum wifi_security_type
} wlan_ap_t;

// (Re)connect: take the SSID / PSK from the SD card config if it has them, then use the saved ones. Runs in the background, the result is logged
int wlan_connect(void);

// Drop the link, no reconnect until wlan_connect()
int wlan_disconnect(void);

// Connected and DHCP gave an IPv4 address
bool wlan_is_online(void);

// Current link. Quick, fine to call from an app's run() about once a second
int wlan_info(wlan_info_t *info);

// Start a scan in the background, -EBUSY while one is running
int wlan_scan(void);

bool wlan_scan_busy(void);

// Copy the last scan result: one entry per SSID, strongest first, hidden networks left out.
// Returns the number of entries
int wlan_scan_get(wlan_ap_t *out, int max);

// Short name of an enum wifi_security_type, e.g. "WPA2-PSK"
const char *wlan_security_txt(int security);
