/* ESP32 mock platform - subset of ESP-IDF <esp_event.h>.
 * Like the real default event loop, handlers run on a separate thread. */
#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef const char *esp_event_base_t;
typedef void *esp_event_handler_instance_t;
typedef void (*esp_event_handler_t)(void *arg, esp_event_base_t base, int32_t id, void *data);

#define ESP_EVENT_ANY_ID (-1)
#define ESP_EVENT_DECLARE_BASE(id) extern esp_event_base_t const id

esp_err_t esp_event_loop_create_default(void);
esp_err_t esp_event_handler_instance_register(esp_event_base_t base, int32_t id,
                                              esp_event_handler_t handler, void *arg,
                                              esp_event_handler_instance_t *instance);

#ifdef __cplusplus
}
#endif
