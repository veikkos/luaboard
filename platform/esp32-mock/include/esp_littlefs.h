/* ESP32 mock platform - subset of joltwallet/littlefs <esp_littlefs.h>.
 *
 * On the device the LittleFS partition is mounted into the VFS at base_path
 * and the firmware uses plain fopen/opendir/mkdir/unlink/rename on it. On the
 * host those calls go to the real file system, so the mount is a host folder
 * instead (sim_flash_set_dir() in sim_hw.h), and APPS_MOUNT points there. */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *base_path;
    const char *partition_label;
    const void *partition;
    uint8_t format_if_mount_failed : 1;
    uint8_t read_only : 1;
    uint8_t dont_mount : 1;
    uint8_t grow_on_mount : 1;
} esp_vfs_littlefs_conf_t;

esp_err_t esp_vfs_littlefs_register(const esp_vfs_littlefs_conf_t *conf);
esp_err_t esp_littlefs_info(const char *partition_label, size_t *total_bytes, size_t *used_bytes);
esp_err_t esp_littlefs_format(const char *partition_label);

/* Where the "partition" lives on the host. */
const char *sim_littlefs_mount(void);
#define APPS_MOUNT sim_littlefs_mount()

#ifdef __cplusplus
}
#endif
