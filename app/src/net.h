/*
 * Wi-Fi link: joins the stored network and serves the luaboard protocol on
 * TCP port NET_TCP_PORT to one client at a time (the newest wins). The device
 * announces itself via mDNS as luaboard.local, service _luaboard._tcp.
 */
#pragma once

#include <stdbool.h>

#include <stddef.h>
#include "esp_err.h"

/** Start Wi-Fi if credentials are stored. Call once at boot, after NVS. */
void net_init(void);
/** Reconnects, listener, client accept. Call from the main loop. */
void net_poll(void);
/** Store credentials and (re)connect. Empty ssid = forget them and turn Wi-Fi off. */
esp_err_t net_set_credentials(const char *ssid, const char *password);
/** Status as "key=value" pairs: state, ssid (hex), ip, rssi, port, client. */
void net_status(char *out, size_t len);
/** "off" (no network set), "connecting" (joining, or rejoining) or "connected". */
const char *net_state(void);
/** Signal strength of the network in dBm (negative), 0 if not connected. */
int net_rssi(void);
/** Not joined, and at least one try to join has failed: the network isn't
 *  there (out of range, switched off) or doesn't let it in. It keeps trying. */
bool net_wifi_failing(void);
/** Drop every Wi-Fi client, telling a logged-in one why (pairing changed). */
void net_drop_clients(const char *why);
/** "1.2.3.4" when connected, "-" otherwise. */
const char *net_ip(void);
