/* ESP32 mock platform - <esp_random.h> (hardware RNG on the device). */
#pragma once

#include <stddef.h>
#include <stdint.h>

uint32_t esp_random(void);
void esp_fill_random(void *buf, size_t len);
