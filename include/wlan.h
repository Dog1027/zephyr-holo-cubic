#pragma once

#include <stdbool.h>

/*
 * Wi-Fi station. The SSID and PSK come from the SD card config (sdconf.h):
 *   wifi_ssid=MyNetwork
 *   wifi_psk=password      (leave it out for an open network)
 * It connects at boot, the driver reconnects on its own after the link drops.
 * Shell: wlan set / status / forget / connect / disconnect / scan
 */

// (Re)connect with the SSID / PSK in the config. Runs in the background, the result is logged
int wlan_connect(void);

// Drop the link, no reconnect until wlan_connect()
int wlan_disconnect(void);

// Connected and DHCP gave an IPv4 address
bool wlan_is_online(void);
