/* ESP32 mock platform - power management (esp_pm.h): see the header. */
#include "esp_pm.h"

#include <stdatomic.h>
#include <stdlib.h>

#include "sim_hw.h"

struct esp_pm_lock { esp_pm_lock_type_t type; };

static atomic_bool s_light_enabled;      /* esp_pm_configure asked for light sleep */
static atomic_int s_no_light_sleep;      /* ESP_PM_NO_LIGHT_SLEEP locks held */

esp_err_t esp_pm_configure(const void *config)
{
    const esp_pm_config_t *c = config;
    if (!c || c->min_freq_mhz > c->max_freq_mhz) return ESP_ERR_INVALID_ARG;
    atomic_store(&s_light_enabled, c->light_sleep_enable);
    return ESP_OK;
}

esp_err_t esp_pm_lock_create(esp_pm_lock_type_t lock_type, int arg, const char *name, esp_pm_lock_handle_t *out_handle)
{
    (void)arg;
    (void)name;
    if (!out_handle) return ESP_ERR_INVALID_ARG;
    *out_handle = malloc(sizeof **out_handle);
    if (!*out_handle) return ESP_ERR_NO_MEM;
    (*out_handle)->type = lock_type;
    return ESP_OK;
}

esp_err_t esp_pm_lock_acquire(esp_pm_lock_handle_t handle)
{
    if (!handle) return ESP_ERR_INVALID_ARG;
    if (handle->type == ESP_PM_NO_LIGHT_SLEEP) atomic_fetch_add(&s_no_light_sleep, 1);
    return ESP_OK;
}

esp_err_t esp_pm_lock_release(esp_pm_lock_handle_t handle)
{
    if (!handle) return ESP_ERR_INVALID_ARG;
    if (handle->type == ESP_PM_NO_LIGHT_SLEEP) {
        if (atomic_load(&s_no_light_sleep) <= 0) return ESP_ERR_INVALID_STATE;
        atomic_fetch_sub(&s_no_light_sleep, 1);
    }
    return ESP_OK;
}

bool sim_light_sleep_allowed(void) { return atomic_load(&s_light_enabled) && atomic_load(&s_no_light_sleep) == 0; }

/* ---- esp_sleep.h --------------------------------------------------------- */
#include "esp_sleep.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static int64_t s_timer_us = -1;      /* the timer wake source, if one was enabled */

esp_err_t esp_sleep_enable_timer_wakeup(uint64_t time_in_us)
{
    s_timer_us = (int64_t)time_in_us;
    return ESP_OK;
}

esp_err_t esp_sleep_enable_ext1_wakeup_io(uint64_t io_mask, esp_sleep_ext1_wakeup_mode_t level_mode)
{
    (void)io_mask;
    (void)level_mode;
    return ESP_OK;
}

/* The simulator ends a deep sleep by its timer only. */
uint32_t esp_sleep_get_wakeup_causes(void)
{
    return esp_reset_reason() == ESP_RST_DEEPSLEEP ? 1u << ESP_SLEEP_WAKEUP_TIMER : 0;
}

void esp_deep_sleep_start(void)
{
    sim_deep_sleep(s_timer_us);
    for (;;) vTaskDelay(1);          /* parks: a sleeping chip runs nothing */
}
