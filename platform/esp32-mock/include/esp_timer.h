/* ESP32 mock platform - subset of ESP-IDF <esp_timer.h>. */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Microseconds since boot. */
int64_t esp_timer_get_time(void);

#ifdef __cplusplus
}
#endif
