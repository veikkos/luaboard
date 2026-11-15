/* ESP32 mock platform - minimal FreeRTOS task API. */
#pragma once

/* Like the real FreeRTOS: FreeRTOS.h must be included first (the real
   task.h has an #error for this, so the mock must not hide the mistake). */
#ifndef configTICK_RATE_HZ
#error "include FreeRTOS.h must appear in source files before include task.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

void vTaskDelay(TickType_t ticks);
TickType_t xTaskGetTickCount(void);

#ifdef __cplusplus
}
#endif
