/* ESP32 mock platform - the microSD card as a host folder: a subset of
   ESP-IDF's <esp_vfs_fat.h>, <driver/sdspi_host.h> and <sdmmc_cmd.h>.

   The card is a folder on this computer (sim_sd_set in sim_hw.h), so its
   files are plain host files: SD_MOUNT, the path the firmware reaches them
   by, points there while a card is mounted (and nowhere otherwise). The
   mock refuses to work while the firmware drives the SPI bus's MISO line,
   which on the CoreS3 doubles as the display's D/C: the card couldn't
   answer on it. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct { int slot; int max_freq_khz; } sdmmc_host_t;
#define SDSPI_HOST_DEFAULT() { .slot = SPI2_HOST, .max_freq_khz = 20000 }

typedef struct { spi_host_device_t host_id; gpio_num_t gpio_cs; } sdspi_device_config_t;
#define SDSPI_DEVICE_CONFIG_DEFAULT() { .host_id = SPI2_HOST, .gpio_cs = GPIO_NUM_NC }

typedef struct {
    bool format_if_mount_failed;
    int max_files;
    size_t allocation_unit_size;
} esp_vfs_fat_mount_config_t;

typedef struct sdmmc_card sdmmc_card_t;

/** ESP_OK with a FAT card in the slot; ESP_FAIL for a card with no FAT file
 *  system; ESP_ERR_TIMEOUT for an empty slot. */
esp_err_t esp_vfs_fat_sdspi_mount(const char *base_path, const sdmmc_host_t *host, const sdspi_device_config_t *slot,
                                  const esp_vfs_fat_mount_config_t *mount_config, sdmmc_card_t **out_card);
esp_err_t esp_vfs_fat_sdcard_unmount(const char *base_path, sdmmc_card_t *card);
esp_err_t esp_vfs_fat_info(const char *base_path, uint64_t *out_total_bytes, uint64_t *out_free_bytes);
/** Is the card still there and answering? */
esp_err_t sdmmc_get_status(sdmmc_card_t *card);

const char *sim_sd_mount(void);
#define SD_MOUNT sim_sd_mount()

#ifdef __cplusplus
}
#endif
