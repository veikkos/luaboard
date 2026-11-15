/* ESP32 mock platform - subset of ESP-IDF <esp_sleep.h>: wake sources are
   accepted and forgotten, and deep sleep ends as a board that is powered
   down (sim_power_off in sim_hw.h): the simulator turns it on again. */
#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { ESP_EXT1_WAKEUP_ANY_LOW, ESP_EXT1_WAKEUP_ANY_HIGH } esp_sleep_ext1_wakeup_mode_t;
typedef enum {
    ESP_SLEEP_WAKEUP_UNDEFINED, ESP_SLEEP_WAKEUP_ALL, ESP_SLEEP_WAKEUP_EXT0, ESP_SLEEP_WAKEUP_EXT1,
    ESP_SLEEP_WAKEUP_TIMER,
} esp_sleep_wakeup_cause_t;

esp_err_t esp_sleep_enable_timer_wakeup(uint64_t time_in_us);
esp_err_t esp_sleep_enable_ext1_wakeup_io(uint64_t io_mask, esp_sleep_ext1_wakeup_mode_t level_mode);
/** A bit per esp_sleep_wakeup_cause_t: the timer's after a deep sleep (the
 *  only way the simulator ends one), else 0. */
uint32_t esp_sleep_get_wakeup_causes(void);
void esp_deep_sleep_start(void) __attribute__((noreturn));

#ifdef __cplusplus
}
#endif
