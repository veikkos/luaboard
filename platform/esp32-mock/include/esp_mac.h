/* ESP32 mock platform - <esp_mac.h>: the chip's factory MAC address. */
#pragma once

#include <stdint.h>

#include "esp_err.h"

/** The simulated chip's own address: random the first time, then kept in
 *  "<nvs file>.mac", as a chip keeps it in its eFuses (an erase of the
 *  flash doesn't change it). */
esp_err_t esp_efuse_mac_get_default(uint8_t *mac);
