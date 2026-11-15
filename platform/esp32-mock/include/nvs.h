/* ESP32 mock platform - subset of ESP-IDF <nvs.h>. Backed by a host file. */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t nvs_handle_t;
typedef enum { NVS_READONLY, NVS_READWRITE } nvs_open_mode_t;

esp_err_t nvs_open(const char *namespace_name, nvs_open_mode_t open_mode, nvs_handle_t *out_handle);
/** If out_value is NULL, *length is set to the stored size. */
esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *out_value, size_t *length);
esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *value, size_t length);
esp_err_t nvs_erase_key(nvs_handle_t handle, const char *key);
esp_err_t nvs_erase_all(nvs_handle_t handle);
esp_err_t nvs_commit(nvs_handle_t handle);
void nvs_close(nvs_handle_t handle);

#ifdef __cplusplus
}
#endif
