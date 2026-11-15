/* ESP32 mock platform - subset of ESP-IDF <esp_task_wdt.h>.
 * The simulator has no hardware watchdog: these do nothing. */
#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

typedef void *TaskHandle_t;

static inline esp_err_t esp_task_wdt_add(TaskHandle_t task) { (void)task; return ESP_OK; }
static inline esp_err_t esp_task_wdt_delete(TaskHandle_t task) { (void)task; return ESP_OK; }
static inline esp_err_t esp_task_wdt_reset(void) { return ESP_OK; }
