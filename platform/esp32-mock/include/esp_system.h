/* ESP32 mock platform - subset of ESP-IDF <esp_system.h>. */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ESP_RST_UNKNOWN, ESP_RST_POWERON, ESP_RST_EXT, ESP_RST_SW, ESP_RST_PANIC, ESP_RST_INT_WDT,
    ESP_RST_TASK_WDT, ESP_RST_WDT, ESP_RST_DEEPSLEEP, ESP_RST_BROWNOUT, ESP_RST_SDIO,
    ESP_RST_USB, ESP_RST_JTAG, ESP_RST_EFUSE, ESP_RST_PWR_GLITCH, ESP_RST_CPU_LOCKUP,
} esp_reset_reason_t;

/** Why the chip last reset. The simulator sets it (sim_set_reset_reason). */
esp_reset_reason_t esp_reset_reason(void);

/** Reset the chip. In the simulator: start over (sim_set_restart_handler),
 *  keeping the serial link and the flash files, as the chip keeps its USB
 *  port and flash across a software reset. */
void esp_restart(void) __attribute__((noreturn));

/** Free heap reported by the mock (a fixed, ESP32-like figure). */
uint32_t esp_get_free_heap_size(void);

#ifdef __cplusplus
}
#endif
