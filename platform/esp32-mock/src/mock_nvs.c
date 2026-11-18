/*
 * ESP32 mock platform - NVS (non-volatile storage), backed by a host file.
 *
 * File format: sequence of records
 *   u8 ns_len, ns[ns_len], u8 key_len, key[key_len], u32le len, data[len]
 * The whole store is kept in RAM; nvs_commit() rewrites the file.
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sim_hw.h"

#define MAX_HANDLES 8
#define NS_MAX 16    /* NVS limits: 15 chars + NUL */
#define KEY_MAX 16

typedef struct entry {
    char ns[NS_MAX];
    char key[KEY_MAX];
    uint8_t *data;
    size_t len;
    struct entry *next;
} entry_t;

static const char *TAG = "nvs";
static char s_path[512] = "esp32-sim-nvs.bin";
static entry_t *s_entries;
static int s_inited;
static char s_handles[MAX_HANDLES][NS_MAX];
static int s_handle_rw[MAX_HANDLES];
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;

void sim_nvs_set_path(const char *path)
{
    snprintf(s_path, sizeof s_path, "%s", path);
}

/* The chip's MAC address, in a file of its own next to the NVS file. */
esp_err_t esp_efuse_mac_get_default(uint8_t *mac)
{
    char path[sizeof s_path + 8];
    snprintf(path, sizeof path, "%s.mac", s_path);
    FILE *f = fopen(path, "rb");
    if (f) {
        size_t n = fread(mac, 1, 6, f);
        fclose(f);
        if (n == 6) return ESP_OK;
    }
    esp_fill_random(mac, 6);
    mac[0] = (uint8_t)(mac[0] & 0xFC);           /* a unicast address, as Espressif's are */
    f = fopen(path, "wb");
    if (f) {
        fwrite(mac, 1, 6, f);
        fclose(f);
    }
    return ESP_OK;
}

static entry_t *find(const char *ns, const char *key)
{
    for (entry_t *e = s_entries; e; e = e->next)
        if (!strcmp(e->ns, ns) && !strcmp(e->key, key)) return e;
    return NULL;
}

static void free_all(void)
{
    while (s_entries) {
        entry_t *n = s_entries->next;
        free(s_entries->data);
        free(s_entries);
        s_entries = n;
    }
}

static int read_str(FILE *f, char *out, size_t cap)
{
    int n = fgetc(f);
    if (n == EOF || (size_t)n >= cap) return -1;
    if (fread(out, 1, (size_t)n, f) != (size_t)n) return -1;
    out[n] = 0;
    return 0;
}

esp_err_t nvs_flash_init(void)
{
    pthread_mutex_lock(&s_lock);
    free_all();
    FILE *f = fopen(s_path, "rb");
    if (f) {
        for (;;) {
            entry_t e = {0};
            uint8_t lb[4];
            if (read_str(f, e.ns, sizeof e.ns) || read_str(f, e.key, sizeof e.key)) break;
            if (fread(lb, 1, 4, f) != 4) break;
            e.len = lb[0] | (lb[1] << 8) | (lb[2] << 16) | ((size_t)lb[3] << 24);
            e.data = malloc(e.len ? e.len : 1);
            if (!e.data || fread(e.data, 1, e.len, f) != e.len) { free(e.data); break; }
            entry_t *ne = malloc(sizeof *ne);
            *ne = e;
            ne->next = s_entries;
            s_entries = ne;
        }
        fclose(f);
        ESP_LOGI(TAG, "loaded store from %s", s_path);
    } else {
        ESP_LOGI(TAG, "no store at %s, starting empty", s_path);
    }
    s_inited = 1;
    pthread_mutex_unlock(&s_lock);
    return ESP_OK;
}

esp_err_t nvs_flash_erase(void)
{
    pthread_mutex_lock(&s_lock);
    free_all();
    remove(s_path);
    pthread_mutex_unlock(&s_lock);
    return ESP_OK;
}

esp_err_t nvs_open(const char *ns, nvs_open_mode_t mode, nvs_handle_t *out)
{
    if (!s_inited) return ESP_ERR_INVALID_STATE;
    if (!ns || !out || strlen(ns) >= NS_MAX) return ESP_ERR_INVALID_ARG;
    pthread_mutex_lock(&s_lock);
    for (int i = 0; i < MAX_HANDLES; i++) {
        if (!s_handles[i][0]) {
            snprintf(s_handles[i], NS_MAX, "%s", ns);
            s_handle_rw[i] = (mode == NVS_READWRITE);
            *out = (nvs_handle_t)(i + 1);
            pthread_mutex_unlock(&s_lock);
            return ESP_OK;
        }
    }
    pthread_mutex_unlock(&s_lock);
    return ESP_ERR_NO_MEM;
}

static const char *handle_ns(nvs_handle_t h)
{
    if (h < 1 || h > MAX_HANDLES || !s_handles[h - 1][0]) return NULL;
    return s_handles[h - 1];
}

esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *out_value, size_t *length)
{
    if (!key || !length) return ESP_ERR_INVALID_ARG;
    pthread_mutex_lock(&s_lock);
    const char *ns = handle_ns(h);
    esp_err_t rc = ESP_OK;
    entry_t *e = ns ? find(ns, key) : NULL;
    if (!ns) rc = ESP_ERR_INVALID_ARG;
    else if (!e) rc = ESP_ERR_NVS_NOT_FOUND;
    else if (!out_value) *length = e->len;
    else if (*length < e->len) rc = ESP_ERR_NVS_INVALID_LENGTH;
    else { memcpy(out_value, e->data, e->len); *length = e->len; }
    pthread_mutex_unlock(&s_lock);
    return rc;
}

esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *value, size_t length)
{
    if (!key || strlen(key) >= KEY_MAX || (!value && length)) return ESP_ERR_INVALID_ARG;
    pthread_mutex_lock(&s_lock);
    const char *ns = handle_ns(h);
    if (!ns || !s_handle_rw[h - 1]) { pthread_mutex_unlock(&s_lock); return ESP_ERR_INVALID_ARG; }
    entry_t *e = find(ns, key);
    if (!e) {
        e = calloc(1, sizeof *e);
        snprintf(e->ns, NS_MAX, "%s", ns);
        snprintf(e->key, KEY_MAX, "%s", key);
        e->next = s_entries;
        s_entries = e;
    }
    free(e->data);
    e->data = malloc(length ? length : 1);
    memcpy(e->data, value, length);
    e->len = length;
    pthread_mutex_unlock(&s_lock);
    return ESP_OK;
}

esp_err_t nvs_erase_key(nvs_handle_t h, const char *key)
{
    pthread_mutex_lock(&s_lock);
    const char *ns = handle_ns(h);
    esp_err_t rc = ESP_ERR_NVS_NOT_FOUND;
    for (entry_t **pp = &s_entries; ns && *pp; pp = &(*pp)->next) {
        if (!strcmp((*pp)->ns, ns) && !strcmp((*pp)->key, key)) {
            entry_t *dead = *pp;
            *pp = dead->next;
            free(dead->data);
            free(dead);
            rc = ESP_OK;
            break;
        }
    }
    pthread_mutex_unlock(&s_lock);
    return rc;
}

esp_err_t nvs_erase_all(nvs_handle_t h)
{
    pthread_mutex_lock(&s_lock);
    const char *ns = handle_ns(h);
    for (entry_t **pp = &s_entries; ns && *pp; ) {
        if (!strcmp((*pp)->ns, ns)) {
            entry_t *dead = *pp;
            *pp = dead->next;
            free(dead->data);
            free(dead);
        } else {
            pp = &(*pp)->next;
        }
    }
    pthread_mutex_unlock(&s_lock);
    return ns ? ESP_OK : ESP_ERR_INVALID_ARG;
}

static void write_str(FILE *f, const char *s)
{
    uint8_t n = (uint8_t)strlen(s);
    fputc(n, f);
    fwrite(s, 1, n, f);
}

esp_err_t nvs_commit(nvs_handle_t h)
{
    pthread_mutex_lock(&s_lock);
    if (!handle_ns(h)) { pthread_mutex_unlock(&s_lock); return ESP_ERR_INVALID_ARG; }
    char tmp[600];
    snprintf(tmp, sizeof tmp, "%s.tmp", s_path);
    FILE *f = fopen(tmp, "wb");
    if (!f) { pthread_mutex_unlock(&s_lock); return ESP_FAIL; }
    for (entry_t *e = s_entries; e; e = e->next) {
        write_str(f, e->ns);
        write_str(f, e->key);
        uint8_t lb[4] = { e->len & 0xff, (e->len >> 8) & 0xff, (e->len >> 16) & 0xff, (e->len >> 24) & 0xff };
        fwrite(lb, 1, 4, f);
        fwrite(e->data, 1, e->len, f);
    }
    int ok = (fclose(f) == 0) && (rename(tmp, s_path) == 0);
    pthread_mutex_unlock(&s_lock);
    return ok ? ESP_OK : ESP_FAIL;
}

void nvs_close(nvs_handle_t h)
{
    pthread_mutex_lock(&s_lock);
    if (h >= 1 && h <= MAX_HANDLES) s_handles[h - 1][0] = 0;
    pthread_mutex_unlock(&s_lock);
}
