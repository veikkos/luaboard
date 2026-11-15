/*
 * Bluetooth LE central on top of the NimBLE host: scanning, connections to
 * peripherals, GATT reads, writes and notifications. Used by the Lua `ble`
 * module (lua_ble.c); everything here is non-blocking for the main loop.
 *
 * Characteristics are referred to by (connection, index), the index into the
 * connection's list as discovered. Requests return a token to poll.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "host/ble_hs.h"

#define BLE_MAX_CONNS 3
#define BLE_AD_MAX    62     /* advertising data + scan response */
#define BLE_VALUE_MAX 512

/** Main loop: start queued scans and connections, time out requests. */
void ble_poll(void);
/** A script ended: stop scanning, close its connections, drop what's queued. */
void ble_reset(void);
/** Scanning, connecting or connected: the radio is in use (no light sleep). */
bool ble_busy(void);

/* ---- scanning ---------------------------------------------------------- */
bool ble_scan_start(bool active, char *err, size_t errlen);
void ble_scan_stop(void);

typedef struct {
    ble_addr_t addr;
    int rssi;
    uint8_t ad[BLE_AD_MAX];
    size_t ad_len;
} ble_adv_t;
/** The next device heard from since it was last returned. */
bool ble_adv_next(ble_adv_t *out);

/* ---- connections -------------------------------------------------------- */
typedef enum { BLE_CONNECTING, BLE_CONNECTED, BLE_CLOSED } ble_state_t;

/** Start connecting for an app. Returns a connection id (0..BLE_MAX_CONNS-1)
 *  or -1. A device bonded with another app refuses ("paired with app ..."). */
int ble_connect(const ble_addr_t *addr, const char *app, char *err, size_t errlen);
/** Delete the bonds an app made (uninstall, Clear data); NULL: every app's. */
void ble_forget_app(const char *app);
ble_state_t ble_state(int id, char *err, size_t errlen);
void ble_close(int id);

typedef struct {
    ble_uuid_any_t svc, uuid;
    uint8_t props;           /* BLE_GATT_CHR_PROP_* */
} ble_chr_info_t;
/** Characteristics of a connected peer (0 while still connecting). */
int ble_chr_count(int id);
bool ble_chr_info(int id, int chr, ble_chr_info_t *out);
/** Index of the characteristic, -1 if the peer has none such. */
int ble_chr_find(int id, const ble_uuid_any_t *svc, const ble_uuid_any_t *uuid);

/* ---- requests ------------------------------------------------------------ */
/** Each returns a token (> 0), or 0 with err set. */
int ble_read(int id, int chr, char *err, size_t errlen);
int ble_write(int id, int chr, const void *data, size_t len, bool response, char *err, size_t errlen);
int ble_subscribe(int id, int chr, bool on, char *err, size_t errlen);
/** 0: pending; 1: done (a read's value in buf/len); -1: failed (err). The
    token is spent once it reports done or failed. */
int ble_result(int token, uint8_t *buf, size_t cap, size_t *len, char *err, size_t errlen);

/** The next notification or indication received on connection id. */
bool ble_notified(int id, int *chr, uint8_t *buf, size_t cap, size_t *len);

/* ---- UUIDs and addresses ------------------------------------------------ */
/** "180f", "0x180F", or 128-bit text (with or without dashes; the Bluetooth
    base UUID collapses to the 16-bit form). */
bool ble_uuid_parse(const char *s, ble_uuid_any_t *out);
/** 16-bit: "180f"; 128-bit: "000018ef-5348-494d-414e-4f5f424c4500". */
void ble_uuid_format(const ble_uuid_any_t *u, char out[BLE_UUID_STR_LEN]);
/** "c7:18:2c:93:8f:cc" (case-insensitive). */
bool ble_addr_parse(const char *s, ble_addr_t *out);
void ble_addr_format(const ble_addr_t *a, char out[18]);
