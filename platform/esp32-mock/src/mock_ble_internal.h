/* Between the NimBLE mock (mock_ble.c) and its host Bluetooth backend
   (mock_ble_host.m, macOS CoreBluetooth). Not part of the API. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "host/ble_hs.h"

#define MOCK_BLE_MAX_CHRS 48

/* One characteristic of a connected peer, in discovery order (grouped by
   service). The backend refers to characteristics by their index here. */
typedef struct {
    ble_uuid_any_t svc;
    ble_uuid_any_t uuid;
    uint8_t props;
} mock_ble_chr_t;

/* ---- core -> backend (all return at once; results come back below) ---- */
bool mock_ble_host_start(void);
void mock_ble_host_scan(bool on, bool active, bool duplicates);
/** Connect `link` to the device seen with address `addr` (as written). */
void mock_ble_host_connect(int link, const uint8_t addr[6]);
void mock_ble_host_disconnect(int link);
void mock_ble_host_read(int link, int chr);
void mock_ble_host_write(int link, int chr, const uint8_t *data, size_t len, bool response);
void mock_ble_host_subscribe(int link, int chr, bool on);

/* ---- backend -> core (any thread) ------------------------------------- */
void mock_ble_host_adv(const uint8_t addr[6], int rssi, const uint8_t *ad, size_t len);
/** Connected and discovered: its characteristics. */
void mock_ble_host_connected(int link, const mock_ble_chr_t *chrs, int n);
/** Lost, or never connected: reason is an HCI error code. */
void mock_ble_host_disconnected(int link, int hci_reason);
/** att_err 0: success. */
void mock_ble_host_read_done(int link, int chr, int att_err, const uint8_t *data, size_t len);
void mock_ble_host_write_done(int link, int chr, int att_err);
void mock_ble_host_subscribe_done(int link, int chr, int att_err);
/** A notification or indication. */
void mock_ble_host_value(int link, int chr, const uint8_t *data, size_t len);
