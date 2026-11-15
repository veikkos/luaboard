/*
 * ESP32 mock platform - subset of ESP-IDF <esp_ota_ops.h>.
 *
 * In the simulator an app image is a simulator executable built for the same
 * board. The two app slots are files next to the NVS file, and a reboot runs
 * the slot the (simulated) bootloader picks, with rollback as on the device
 * (CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE). See mock_ota.c.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_app_desc.h"
#include "esp_err.h"
#include "esp_partition.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t esp_ota_handle_t;

#define OTA_SIZE_UNKNOWN           0xffffffff
#define OTA_WITH_SEQUENTIAL_WRITES 0xfffffffe

#define ESP_ERR_OTA_BASE                   0x1500
#define ESP_ERR_OTA_PARTITION_CONFLICT     (ESP_ERR_OTA_BASE + 0x01)
#define ESP_ERR_OTA_SELECT_INFO_INVALID    (ESP_ERR_OTA_BASE + 0x02)
#define ESP_ERR_OTA_VALIDATE_FAILED        (ESP_ERR_OTA_BASE + 0x03)
#define ESP_ERR_OTA_ROLLBACK_INVALID_STATE (ESP_ERR_OTA_BASE + 0x06)

typedef enum {
    ESP_OTA_IMG_NEW = 0x0,
    ESP_OTA_IMG_PENDING_VERIFY = 0x1,
    ESP_OTA_IMG_VALID = 0x2,
    ESP_OTA_IMG_INVALID = 0x3,
    ESP_OTA_IMG_ABORTED = 0x4,
    ESP_OTA_IMG_UNDEFINED = 0xFFFFFFFF,
} esp_ota_img_states_t;

const esp_partition_t *esp_ota_get_running_partition(void);
const esp_partition_t *esp_ota_get_boot_partition(void);
const esp_partition_t *esp_ota_get_next_update_partition(const esp_partition_t *start_from);

esp_err_t esp_ota_begin(const esp_partition_t *partition, size_t image_size, esp_ota_handle_t *out_handle);
esp_err_t esp_ota_write(esp_ota_handle_t handle, const void *data, size_t size);
esp_err_t esp_ota_end(esp_ota_handle_t handle);
esp_err_t esp_ota_abort(esp_ota_handle_t handle);
esp_err_t esp_ota_set_boot_partition(const esp_partition_t *partition);

esp_err_t esp_ota_get_partition_description(const esp_partition_t *partition, esp_app_desc_t *app_desc);
esp_err_t esp_ota_get_state_partition(const esp_partition_t *partition, esp_ota_img_states_t *ota_state);
esp_err_t esp_ota_mark_app_valid_cancel_rollback(void);

#ifdef __cplusplus
}
#endif
