/*
 * ESP32 mock platform - LittleFS partition as a host folder.
 *
 * The folder is the partition's contents: one subfolder per installed app.
 * Capacity and usage are reported like the device's partition (size from
 * app/idf/partitions.csv, usage rounded up to 4 KiB blocks), so "flash full"
 * behaves the same.
 */
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_littlefs.h"
#include "esp_log.h"
#include "sim_hw.h"

#define PARTITION_SIZE 0x9E0000   /* "apps" in app/idf/partitions.csv */
#define BLOCK 4096

static const char *TAG = "littlefs";
static char s_dir[512] = "esp32-sim-flash";

void sim_flash_set_dir(const char *dir)
{
    snprintf(s_dir, sizeof s_dir, "%s", dir);
    size_t n = strlen(s_dir);
    while (n > 1 && s_dir[n - 1] == '/') s_dir[--n] = 0;
}

const char *sim_littlefs_mount(void) { return s_dir; }

esp_err_t esp_vfs_littlefs_register(const esp_vfs_littlefs_conf_t *conf)
{
    (void)conf;
    if (mkdir(s_dir, 0755) != 0 && errno != EEXIST) {
        ESP_LOGE(TAG, "cannot create %s: %s", s_dir, strerror(errno));
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "apps partition: folder %s", s_dir);
    return ESP_OK;
}

/* Sum of file sizes under dir, each rounded up to a block, plus a block per folder. */
static size_t usage(const char *dir, int depth)
{
    size_t total = BLOCK;
    DIR *d = opendir(dir);
    if (!d) return 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char p[1024];
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(p, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) { if (depth < 4) total += usage(p, depth + 1); }
        else total += ((size_t)st.st_size + BLOCK - 1) / BLOCK * BLOCK;
    }
    closedir(d);
    return total;
}

esp_err_t esp_littlefs_info(const char *label, size_t *total, size_t *used)
{
    (void)label;
    if (total) *total = PARTITION_SIZE;
    if (used) *used = usage(s_dir, 0);
    return ESP_OK;
}

static void wipe(const char *dir, int depth)
{
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char p[1024];
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        struct stat st;
        if (lstat(p, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) { if (depth < 4) { wipe(p, depth + 1); rmdir(p); } }
        else unlink(p);
    }
    closedir(d);
}

esp_err_t esp_littlefs_format(const char *label)
{
    (void)label;
    wipe(s_dir, 0);
    return ESP_OK;
}
