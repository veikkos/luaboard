/* ESP32 mock platform - power management: the configuration is accepted,
   and the locks are counted so the simulator can say whether the firmware
   allows light sleep (sim_light_sleep_allowed in sim_hw.h). Nothing sleeps. */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int max_freq_mhz;
    int min_freq_mhz;
    bool light_sleep_enable;
} esp_pm_config_t;

typedef enum { ESP_PM_CPU_FREQ_MAX, ESP_PM_APB_FREQ_MAX, ESP_PM_NO_LIGHT_SLEEP, ESP_PM_LOCK_MAX } esp_pm_lock_type_t;
typedef struct esp_pm_lock *esp_pm_lock_handle_t;

esp_err_t esp_pm_configure(const void *config);
esp_err_t esp_pm_lock_create(esp_pm_lock_type_t lock_type, int arg, const char *name, esp_pm_lock_handle_t *out_handle);
esp_err_t esp_pm_lock_acquire(esp_pm_lock_handle_t handle);
esp_err_t esp_pm_lock_release(esp_pm_lock_handle_t handle);

#ifdef __cplusplus
}
#endif
