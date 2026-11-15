/* ESP32 mock platform - the microSD card as a host folder (esp_vfs_fat.h). */
#include "esp_vfs_fat.h"

#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "sim_hw.h"

#define CARD_SIZE (1024ull * 1024 * 1024)        /* a 1 GB card */
#define CLUSTER   16384

struct sdmmc_card { int gen; };

typedef enum { SLOT_EMPTY, SLOT_FAT, SLOT_UNFORMATTED } slot_t;

static pthread_mutex_t s_mu = PTHREAD_MUTEX_INITIALIZER;
static slot_t s_slot = SLOT_EMPTY;
static char s_dir[512];
static int s_gen;                    /* changes with every card put in or taken out */
static struct sdmmc_card s_card;
static bool s_mounted;
static int s_miso = -1;              /* the bus's MISO pin, from the mount's SPI host */

int mock_spi_bus_miso(spi_host_device_t host);   /* mock_spi.c */

void sim_sd_set(const char *what)
{
    pthread_mutex_lock(&s_mu);
    s_gen++;
    if (!strcmp(what, "none") || !strcmp(what, "-") || !*what) s_slot = SLOT_EMPTY;
    else if (!strcmp(what, "unformatted")) s_slot = SLOT_UNFORMATTED;
    else {
        s_slot = SLOT_FAT;
        snprintf(s_dir, sizeof s_dir, "%s", what);
        mkdir(s_dir, 0755);                       /* an empty card, if it isn't there */
    }
    pthread_mutex_unlock(&s_mu);
}

void sim_sd_get(char *out, size_t cap)
{
    pthread_mutex_lock(&s_mu);
    snprintf(out, cap, "%s", s_slot == SLOT_EMPTY ? "none" : s_slot == SLOT_UNFORMATTED ? "unformatted" : s_dir);
    pthread_mutex_unlock(&s_mu);
}

bool sim_sd_mounted(void)
{
    pthread_mutex_lock(&s_mu);
    bool m = s_mounted;
    pthread_mutex_unlock(&s_mu);
    return m;
}

/* The card mounted is still the one in the slot. */
static bool present_locked(void) { return s_mounted && s_slot == SLOT_FAT && s_card.gen == s_gen; }

const char *sim_sd_mount(void)
{
    pthread_mutex_lock(&s_mu);
    const char *p = present_locked() ? s_dir : "/no-sd-card-in-the-simulator";
    pthread_mutex_unlock(&s_mu);
    return p;
}

/* The card answers on MISO: the firmware must not be driving that pin. */
static bool line_free(void)
{
    if (s_miso < 0 || sim_gpio_output_level((gpio_num_t)s_miso) < 0) return true;
    fprintf(stderr, "[sim] SD card: GPIO%d (the card's data out) is driven by the firmware\n", s_miso);
    return false;
}

esp_err_t esp_vfs_fat_sdspi_mount(const char *base_path, const sdmmc_host_t *host, const sdspi_device_config_t *slot,
                                  const esp_vfs_fat_mount_config_t *mount_config, sdmmc_card_t **out_card)
{
    (void)base_path;
    if (!host || !slot || !mount_config || !out_card) return ESP_ERR_INVALID_ARG;
    s_miso = mock_spi_bus_miso(slot->host_id);
    if (s_miso < 0) {
        fprintf(stderr, "[sim] SD card: the SPI bus has no MISO line\n");
        return ESP_ERR_INVALID_STATE;
    }
    if (!line_free()) return ESP_ERR_INVALID_STATE;
    pthread_mutex_lock(&s_mu);
    esp_err_t err = s_mounted ? ESP_ERR_INVALID_STATE
                  : s_slot == SLOT_EMPTY ? ESP_ERR_TIMEOUT : s_slot == SLOT_UNFORMATTED ? ESP_FAIL : ESP_OK;
    if (err == ESP_OK) {
        s_card.gen = s_gen;
        s_mounted = true;
        *out_card = &s_card;
    }
    pthread_mutex_unlock(&s_mu);
    return err;
}

esp_err_t esp_vfs_fat_sdcard_unmount(const char *base_path, sdmmc_card_t *card)
{
    (void)base_path;
    if (card != &s_card) return ESP_ERR_INVALID_ARG;
    pthread_mutex_lock(&s_mu);
    s_mounted = false;
    pthread_mutex_unlock(&s_mu);
    return ESP_OK;
}

esp_err_t sdmmc_get_status(sdmmc_card_t *card)
{
    if (card != &s_card) return ESP_ERR_INVALID_ARG;
    if (!line_free()) return ESP_ERR_INVALID_STATE;
    pthread_mutex_lock(&s_mu);
    bool ok = present_locked();
    pthread_mutex_unlock(&s_mu);
    return ok ? ESP_OK : ESP_ERR_TIMEOUT;
}

/* Bytes the files under dir take on the card, each rounded up to a cluster. */
static uint64_t used(const char *dir, int depth)
{
    uint64_t total = 0;
    DIR *d = opendir(dir);
    if (!d) return 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char p[1024];
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(p, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) total += CLUSTER + (depth < 8 ? used(p, depth + 1) : 0);
        else total += ((uint64_t)st.st_size + CLUSTER - 1) / CLUSTER * CLUSTER;
    }
    closedir(d);
    return total;
}

esp_err_t esp_vfs_fat_info(const char *base_path, uint64_t *out_total_bytes, uint64_t *out_free_bytes)
{
    (void)base_path;
    if (!line_free()) return ESP_ERR_INVALID_STATE;
    pthread_mutex_lock(&s_mu);
    bool ok = present_locked();
    char dir[512];
    snprintf(dir, sizeof dir, "%s", s_dir);
    pthread_mutex_unlock(&s_mu);
    if (!ok) return ESP_ERR_INVALID_STATE;
    uint64_t u = used(dir, 0);
    *out_total_bytes = CARD_SIZE;
    *out_free_bytes = u < CARD_SIZE ? CARD_SIZE - u : 0;
    return ESP_OK;
}
