/*
 * Simulated Bluetooth LE peripherals described in JSON files, so an app can
 * bring a stand-in for its device along without any C (--ble-device FILE):
 *
 *   {
 *     "id": "sensor",                  name for the controls (default: the file's name)
 *     "name": "Sensor 1",              advertised name
 *     "addr": "c7:18:2c:51:4d:01",     random static if the top bits are 11
 *     "present": true,                 in range from the start (default)
 *     "rssi": -60,
 *     "adv": "02 01 06 ...",           advertising data as hex (default: flags + name)
 *     "characteristics": [
 *       { "service": "180f", "uuid": "2a19", "props": "read notify",
 *         "value": "46",               hex, or "text": "..." for UTF-8
 *         "pairing": true,             reads and writes need a paired link
 *         "on_subscribe": ["04 ff ff", "value"] }   sent when subscribed ("value" = its value)
 *     ]
 *   }
 *
 * props: read, write, write_nr, notify, indicate. A written value is kept
 * (if it's writable). Controls (--set and the control channel), with <chr> a
 * characteristic UUID:
 *
 *   ble=<id>:on|off              in range or not
 *   ble_value=<id>:<chr>:<hex>   a new value, notified to subscribers
 *   ble_notify=<id>:<chr>:<hex>  a notification without changing the value
 *   ble_rssi=<id>:<dBm>
 */
#pragma once

#include <stdbool.h>

/** Load a description. False (and why, on stderr) if it can't be used. */
bool ble_device_load(const char *path);
/** A control line for the devices; false if it isn't one of theirs or is invalid. */
bool ble_device_set(const char *key, const char *val);
/** Help text for the controls. */
const char *ble_device_set_help(void);
/** Bring all of them in range, or take them away (the window's D key). */
void ble_device_toggle_all(void);
