#include "esp_vfs_fat.h"   /* first: the simulator's mock sets SD_MOUNT */
#include "sdcard.h"

#include "app_config.h"
#include "board.h"
#include "driver/gpio.h"
#include "driver/sdspi_host.h"
#include "esp_log.h"
#include "hw.h"
#include "sdmmc_cmd.h"

static const char *TAG = "sd";

static sdmmc_card_t *s_card;
static const char *s_failed;         /* why the last mount failed, NULL if none did */
static uint32_t s_failed_at;

/* VERIFY on hardware: the pin as an input lets the card drive it, and the
   display's D/C is back (an output, at the level the LCD code sets before
   each transfer) afterwards. */
void sdcard_begin(void)
{
    /* Pulled up while it's the card's line, as the SD bus wants it: an
       empty slot then reads as no answer, not as noise. */
    gpio_set_pull_mode(BOARD_SD_MISO, GPIO_PULLUP_ONLY);
    gpio_set_direction(BOARD_SD_MISO, GPIO_MODE_INPUT);
}

void sdcard_end(void)
{
    gpio_set_direction(BOARD_LCD_DC, GPIO_MODE_OUTPUT);
    gpio_set_pull_mode(BOARD_LCD_DC, GPIO_FLOATING);
}

/* Without a mounted card its select line is ours, held high: the card
   stays off the bus the display uses. */
static void deselect(void)
{
    gpio_set_level(BOARD_SD_CS, 1);              /* the level first: never low for a moment */
    gpio_set_direction(BOARD_SD_CS, GPIO_MODE_OUTPUT);
}

const char *sdcard_mount(void)
{
    if (s_card) return NULL;
    uint32_t now = hw_millis();
    if (s_failed && now - s_failed_at < SD_RETRY_MS) return s_failed;
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = BOARD_LCD_SPI_HOST;
    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.host_id = BOARD_LCD_SPI_HOST;
    slot.gpio_cs = BOARD_SD_CS;
    /* Never formats a card: one that isn't FAT is refused. */
    esp_vfs_fat_mount_config_t cfg = { .format_if_mount_failed = false, .max_files = 2, .allocation_unit_size = 16 * 1024 };
    esp_err_t err = esp_vfs_fat_sdspi_mount(SD_MOUNT, &host, &slot, &cfg, &s_card);
    if (err == ESP_OK) {
        s_failed = NULL;
        return NULL;
    }
    s_card = NULL;
    deselect();
    s_failed_at = now;
    /* ESP_FAIL is the file system not mounting on a card that answered:
       no FAT on it, or one that can't be read. */
    s_failed = err == ESP_FAIL ? "the card can't be read as FAT (it has to be FAT16 or FAT32)" : "no card";
    ESP_LOGI(TAG, "%s (%s)", s_failed, esp_err_to_name(err));
    return s_failed;
}

void sdcard_unmount(void)
{
    if (!s_card) return;
    esp_vfs_fat_sdcard_unmount(SD_MOUNT, s_card);
    s_card = NULL;
    deselect();
}

void sdcard_check(void)
{
    if (s_card && sdmmc_get_status(s_card) != ESP_OK) {
        ESP_LOGI(TAG, "the card is gone");
        sdcard_unmount();
        s_failed = NULL;             /* the next call looks for a card at once */
    }
}

bool sdcard_info(uint64_t *total, uint64_t *free_bytes)
{
    return s_card && esp_vfs_fat_info(SD_MOUNT, total, free_bytes) == ESP_OK;
}

const char *sdcard_root(void) { return SD_MOUNT; }
