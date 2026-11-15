/* ESP32 mock platform - minimal FreeRTOS surface used by the app. */
#pragma once

#include <stdint.h>

typedef uint32_t TickType_t;
typedef int BaseType_t;

#define configTICK_RATE_HZ   100   /* ESP-IDF default: 10 ms tick */
#define portTICK_PERIOD_MS   (1000 / configTICK_RATE_HZ)
#define portMAX_DELAY        ((TickType_t)0xffffffffUL)
#define pdMS_TO_TICKS(ms)    ((TickType_t)(((uint64_t)(ms) * configTICK_RATE_HZ) / 1000))
#define pdTRUE  1
#define pdFALSE 0
