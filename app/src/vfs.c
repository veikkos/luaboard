#include "esp_littlefs.h"   /* first: the simulator's mock sets APPS_MOUNT */
#include "vfs.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "app_config.h"
#include "crc32.h"
#include "esp_log.h"
#include "hw.h"

static const char *TAG = "vfs";

typedef struct {
    char path[VFS_MAX_NAME];
    uint8_t *data;
    size_t len;
} ram_file_t;

static ram_file_t s_ram[VFS_RAM_FILES];
static bool s_mounted;
static uint32_t s_gen = 1;

uint32_t vfs_generation(void) { return s_gen; }
static void changed(void) { s_gen++; }

/* ------------------------------------------------------------- names */

static bool valid_part(const char *s, size_t n, size_t max)
{
    if (n == 0 || n >= max || s[0] == '.') return false;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
        if (!ok) return false;
    }
    return true;
}

bool vfs_valid_app(const char *app) { return app && valid_part(app, strlen(app), VFS_APP_NAME_MAX); }

bool vfs_valid_path(const char *path)
{
    const char *slash = path ? strchr(path, '/') : NULL;
    if (!slash) return false;
    size_t n = strlen(slash + 1);
    /* "<file>.tmp" is the temporary name of <file> while it is written. */
    if (n >= 4 && !strcmp(slash + 1 + n - 4, ".tmp")) return false;
    return valid_part(path, (size_t)(slash - path), VFS_APP_NAME_MAX) &&
           valid_part(slash + 1, n, VFS_FILE_NAME_MAX);
}

void vfs_app_of(const char *path, char *app, size_t cap)
{
    const char *slash = strchr(path, '/');
    size_t n = slash ? (size_t)(slash - path) : strlen(path);
    snprintf(app, cap, "%.*s", (int)n, path);
}

static bool in_app(const char *path, const char *app)
{
    size_t n = strlen(app);
    return !strncmp(path, app, n) && path[n] == '/';
}

static void flash_path(char *out, size_t cap, const char *rel)
{
    snprintf(out, cap, "%s/%s", APPS_MOUNT, rel);
}

/* --------------------------------------------------------------- RAM */

static ram_file_t *ram_find(const char *path)
{
    for (int i = 0; i < VFS_RAM_FILES; i++)
        if (s_ram[i].data && !strcmp(s_ram[i].path, path)) return &s_ram[i];
    return NULL;
}

static size_t ram_used(void)
{
    size_t t = 0;
    for (int i = 0; i < VFS_RAM_FILES; i++) if (s_ram[i].data) t += s_ram[i].len;
    return t;
}

static void ram_drop(ram_file_t *f)
{
    free(f->data);
    memset(f, 0, sizeof *f);
}

static bool ram_has_app(const char *app)
{
    for (int i = 0; i < VFS_RAM_FILES; i++)
        if (s_ram[i].data && in_app(s_ram[i].path, app)) return true;
    return false;
}

esp_err_t vfs_write(const char *path, const uint8_t *data, size_t len)
{
    if (!vfs_valid_path(path)) return ESP_ERR_INVALID_ARG;
    char app[VFS_APP_NAME_MAX];
    vfs_app_of(path, app, sizeof app);
    if (vfs_app_case_clash(app, NULL, 0)) return ESP_ERR_INVALID_ARG;
    if (len > VFS_FILE_MAX) return ESP_ERR_INVALID_SIZE;
    ram_file_t *f = ram_find(path);
    if (ram_used() - (f ? f->len : 0) + len > VFS_RAM_CAPACITY) return ESP_ERR_NO_MEM;
    if (!f) {
        for (int i = 0; i < VFS_RAM_FILES && !f; i++) if (!s_ram[i].data) f = &s_ram[i];
        if (!f) return ESP_ERR_NO_MEM;
    }
    uint8_t *copy = malloc(len ? len : 1);
    if (!copy) return ESP_ERR_NO_MEM;
    memcpy(copy, data, len);
    free(f->data);
    snprintf(f->path, sizeof f->path, "%s", path);
    f->data = copy;
    f->len = len;
    changed();
    return ESP_OK;
}

/* ------------------------------------------------------------- flash */

static uint8_t *flash_load(const char *path, size_t *len)
{
    char fp[256];
    flash_path(fp, sizeof fp, path);
    FILE *f = fopen(fp, "rb");
    if (!f) return NULL;
    uint8_t *buf = NULL;
    long n = -1;
    if (fseek(f, 0, SEEK_END) == 0) n = ftell(f);
    if (n >= 0 && n <= VFS_FILE_MAX && fseek(f, 0, SEEK_SET) == 0) {
        buf = malloc(n ? (size_t)n : 1);
        if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); buf = NULL; }
    }
    fclose(f);
    if (buf && len) *len = (size_t)n;
    return buf;
}

static long flash_size(const char *path)
{
    char fp[256];
    struct stat st;
    flash_path(fp, sizeof fp, path);
    return stat(fp, &st) == 0 && S_ISREG(st.st_mode) ? (long)st.st_size : -1;
}

/* CRCs of installed files, so LS doesn't read every file every time (the
   PC tools list after each upload, run and install). Known from the data
   when a file is stored, else worked out once when first listed; forgotten
   when the file is replaced or removed. The size double-checks an entry. */
typedef struct crc_entry {
    struct crc_entry *next;
    long size;
    uint32_t crc;
    char path[VFS_MAX_NAME];
} crc_entry_t;

static crc_entry_t *s_crcs;

static void crc_forget(const char *path)
{
    for (crc_entry_t **pp = &s_crcs; *pp; pp = &(*pp)->next) {
        if (!strcmp((*pp)->path, path)) {
            crc_entry_t *e = *pp;
            *pp = e->next;
            free(e);
            return;
        }
    }
}

static void crc_forget_all(void)
{
    while (s_crcs) {
        crc_entry_t *e = s_crcs;
        s_crcs = e->next;
        free(e);
    }
}

static void crc_remember(const char *path, long size, uint32_t crc)
{
    crc_forget(path);
    crc_entry_t *e = malloc(sizeof *e);
    if (!e) return;                       /* just slower next time */
    snprintf(e->path, sizeof e->path, "%s", path);
    e->size = size;
    e->crc = crc;
    e->next = s_crcs;
    s_crcs = e;
}

/* Size and CRC of an installed file, read in pieces if not known yet. */
static bool flash_crc(const char *path, size_t *size, uint32_t *crc)
{
    long n = flash_size(path);
    if (n < 0) return false;
    for (crc_entry_t *e = s_crcs; e; e = e->next) {
        if (!strcmp(e->path, path) && e->size == n) {
            *size = (size_t)n;
            *crc = e->crc;
            return true;
        }
    }
    char fp[256];
    flash_path(fp, sizeof fp, path);
    FILE *f = fopen(fp, "rb");
    if (!f) return false;
    uint8_t buf[512];
    uint32_t c = 0;
    long total = 0;
    size_t k;
    while ((k = fread(buf, 1, sizeof buf, f)) > 0) {
        c = crc32_update(c, buf, k);
        total += (long)k;
    }
    fclose(f);
    if (total != n) return false;
    crc_remember(path, n, c);
    *size = (size_t)n;
    *crc = c;
    return true;
}

/* Write the file fp in folder dir (made if it isn't there) under a
   temporary name, then rename it over the old one (atomic in LittleFS and
   POSIX): a power cut leaves either the old or the new file. */
static esp_err_t write_atomic(const char *dir, const char *fp, const uint8_t *data, size_t len)
{
    char tmp[264];
    if (mkdir(dir, 0755) != 0 && errno != EEXIST) return ESP_FAIL;
    snprintf(tmp, sizeof tmp, "%s.tmp", fp);
    FILE *f = fopen(tmp, "wb");
    if (!f) return ESP_FAIL;
    bool ok = fwrite(data, 1, len, f) == len;
    ok = (fclose(f) == 0) && ok;
    if (ok) ok = rename(tmp, fp) == 0;
    if (!ok) { unlink(tmp); return ESP_ERR_NO_MEM; }
    return ESP_OK;
}

static esp_err_t flash_store(const char *path, const uint8_t *data, size_t len)
{
    char app[VFS_APP_NAME_MAX], dir[256], fp[256];
    vfs_app_of(path, app, sizeof app);
    flash_path(dir, sizeof dir, app);
    flash_path(fp, sizeof fp, path);
    esp_err_t err = write_atomic(dir, fp, data, len);
    if (err != ESP_OK) crc_forget(path);
    else crc_remember(path, (long)len, crc32_calc(data, len));
    return err;
}

static esp_err_t flash_remove(const char *path)
{
    char fp[256];
    flash_path(fp, sizeof fp, path);
    crc_forget(path);
    return unlink(fp) == 0 ? ESP_OK : ESP_ERR_NOT_FOUND;
}

/* Call cb(name) for every installed app folder. */
static void flash_apps(void (*cb)(const char *app, void *ctx), void *ctx)
{
    if (!s_mounted) return;
    DIR *d = opendir(APPS_MOUNT);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!vfs_valid_app(e->d_name)) continue;
        char p[256];
        struct stat st;
        flash_path(p, sizeof p, e->d_name);
        if (stat(p, &st) == 0 && S_ISDIR(st.st_mode)) cb(e->d_name, ctx);
    }
    closedir(d);
}

/* Call cb(path) for every file of an installed app. */
static void flash_files(const char *app, void (*cb)(const char *path, void *ctx), void *ctx)
{
    char dir[256];
    flash_path(dir, sizeof dir, app);
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        size_t n = strlen(e->d_name);
        if (!valid_part(e->d_name, n, VFS_FILE_NAME_MAX)) continue;
        if (n > 4 && !strcmp(e->d_name + n - 4, ".tmp")) continue;   /* interrupted write */
        char path[VFS_MAX_NAME];
        snprintf(path, sizeof path, "%s/%.*s", app, (int)n, e->d_name);   /* n < VFS_FILE_NAME_MAX */
        if (flash_size(path) >= 0) cb(path, ctx);
    }
    closedir(d);
}

/* ---------------------------------------------------------- reading */

uint8_t *vfs_load(const char *path, size_t *len)
{
    if (!vfs_valid_path(path)) return NULL;
    ram_file_t *f = ram_find(path);
    if (f) {
        uint8_t *copy = malloc(f->len ? f->len : 1);
        if (copy) { memcpy(copy, f->data, f->len); if (len) *len = f->len; }
        return copy;
    }
    return s_mounted ? flash_load(path, len) : NULL;
}

bool vfs_exists(const char *path)
{
    if (!vfs_valid_path(path)) return false;
    return ram_find(path) || (s_mounted && flash_size(path) >= 0);
}

esp_err_t vfs_delete(const char *path)
{
    if (!vfs_valid_path(path)) return ESP_ERR_NOT_FOUND;
    bool found = false;
    ram_file_t *f = ram_find(path);
    if (f) { ram_drop(f); found = true; }
    if (s_mounted && flash_remove(path) == ESP_OK) found = true;
    if (!found) return ESP_ERR_NOT_FOUND;
    /* An installed app whose last file is gone: remove its folder too. */
    char app[VFS_APP_NAME_MAX], dir[256];
    vfs_app_of(path, app, sizeof app);
    flash_path(dir, sizeof dir, app);
    rmdir(dir);                           /* fails (fine) while other files remain */
    changed();
    return ESP_OK;
}

/* --------------------------------------------------------- app data */

#define DATA_DIR ".data"   /* not a valid app name, so never listed as one */

static bool data_path(const char *app, char *out, size_t cap)
{
    if (!s_mounted || !vfs_valid_app(app)) return false;
    snprintf(out, cap, "%s/" DATA_DIR "/%s", APPS_MOUNT, app);
    return true;
}

/* Writes are spaced out: one right away if the last was DATA_WRITE_GAP_MS
   ago or more, else the newest data waits in RAM (s_pend) and goes to flash
   that long after the last write (vfs_data_poll), or when the script ends.
   A script calling store.set() every frame would otherwise wear the flash
   out within days. A power cut loses at most that much. */
#define DATA_WRITE_GAP_MS 1000

static struct {
    bool on;
    char app[VFS_APP_NAME_MAX];
    uint8_t *data;                   /* NULL with len 0: delete the file */
    size_t len;
} s_pend;
static uint32_t s_data_last;
static bool s_data_wrote;

static void pend_drop(void)
{
    free(s_pend.data);
    s_pend.data = NULL;
    s_pend.len = 0;
    s_pend.on = false;
}

static bool pend_for(const char *app) { return s_pend.on && !strcmp(s_pend.app, app); }

long vfs_data_size(const char *app)
{
    char p[256];
    struct stat st;
    if (!data_path(app, p, sizeof p)) return -1;
    if (pend_for(app)) return s_pend.len ? (long)s_pend.len : -1;
    if (stat(p, &st) != 0) return -1;
    return (long)st.st_size;
}

long vfs_data_read(const char *app, uint8_t *buf, size_t cap)
{
    char p[256];
    if (!data_path(app, p, sizeof p)) return -1;
    if (pend_for(app)) {
        if (!s_pend.len) return -1;
        size_t n = s_pend.len < cap ? s_pend.len : cap;
        memcpy(buf, s_pend.data, n);
        return (long)n;
    }
    FILE *f = fopen(p, "rb");
    if (!f) return -1;
    size_t n = fread(buf, 1, cap, f);
    fclose(f);
    return (long)n;
}

static esp_err_t data_write(const char *app, const uint8_t *data, size_t len)
{
    char p[256], dir[256];
    if (!data_path(app, p, sizeof p)) return ESP_ERR_INVALID_STATE;
    s_data_last = hw_millis();
    s_data_wrote = true;
    if (!len) { unlink(p); return ESP_OK; }
    snprintf(dir, sizeof dir, "%s/" DATA_DIR, APPS_MOUNT);
    return write_atomic(dir, p, data, len);
}

esp_err_t vfs_data_save(const char *app, const uint8_t *data, size_t len)
{
    char p[256];
    if (!data_path(app, p, sizeof p)) return ESP_ERR_INVALID_STATE;
    if (s_pend.on && !pend_for(app)) vfs_data_flush();
    if (!s_pend.on && (!s_data_wrote || hw_millis() - s_data_last >= DATA_WRITE_GAP_MS))
        return data_write(app, data, len);
    uint8_t *copy = NULL;
    if (len) {
        copy = malloc(len);
        if (!copy) return ESP_ERR_NO_MEM;
        memcpy(copy, data, len);
    }
    pend_drop();
    s_pend.on = true;
    snprintf(s_pend.app, sizeof s_pend.app, "%s", app);
    s_pend.data = copy;
    s_pend.len = len;
    return ESP_OK;
}

void vfs_data_flush(void)
{
    if (!s_pend.on) return;
    if (data_write(s_pend.app, s_pend.data, s_pend.len) != ESP_OK)
        ESP_LOGW(TAG, "saving the stored values of '%s' failed", s_pend.app);
    pend_drop();
}

void vfs_data_poll(void)
{
    if (s_pend.on && hw_millis() - s_data_last >= DATA_WRITE_GAP_MS) vfs_data_flush();
}

void vfs_data_clear(const char *app)
{
    char p[256];
    if (pend_for(app)) pend_drop();
    if (data_path(app, p, sizeof p)) unlink(p);
}

/* ---------------------------------------------------------- listing */

typedef struct {
    char (*names)[VFS_MAX_NAME];
    int n, max;
} name_set_t;

static void set_add(name_set_t *s, const char *name)
{
    for (int i = 0; i < s->n; i++) if (!strcmp(s->names[i], name)) return;
    if (s->n < s->max) snprintf(s->names[s->n++], VFS_MAX_NAME, "%s", name);
}

static int cmp_names(const void *a, const void *b) { return strcmp(a, b); }

static void add_app_cb(const char *app, void *ctx) { set_add(ctx, app); }
static void add_path_cb(const char *path, void *ctx) { set_add(ctx, path); }

/* All app names (RAM and flash), sorted. Caller frees s->names. */
static int collect_apps(name_set_t *s)
{
    s->max = VFS_MAX_APPS;
    s->n = 0;
    s->names = calloc((size_t)s->max, VFS_MAX_NAME);
    if (!s->names) return 0;
    for (int i = 0; i < VFS_RAM_FILES; i++) {
        if (!s_ram[i].data) continue;
        char app[VFS_APP_NAME_MAX];
        vfs_app_of(s_ram[i].path, app, sizeof app);
        set_add(s, app);
    }
    flash_apps(add_app_cb, s);
    qsort(s->names, (size_t)s->n, VFS_MAX_NAME, cmp_names);
    return s->n;
}

/* All file paths of one app (RAM and flash), sorted. Caller frees s->names. */
static int collect_files(const char *app, name_set_t *s)
{
    s->max = VFS_RAM_FILES + 64;
    s->n = 0;
    s->names = calloc((size_t)s->max, VFS_MAX_NAME);
    if (!s->names) return 0;
    for (int i = 0; i < VFS_RAM_FILES; i++)
        if (s_ram[i].data && in_app(s_ram[i].path, app)) set_add(s, s_ram[i].path);
    if (s_mounted) flash_files(app, add_path_cb, s);
    qsort(s->names, (size_t)s->n, VFS_MAX_NAME, cmp_names);
    return s->n;
}

bool vfs_app_case_clash(const char *app, char *other, size_t cap)
{
    name_set_t apps;
    bool clash = false;
    collect_apps(&apps);
    for (int i = 0; i < apps.n && !clash; i++) {
        if (strcasecmp(apps.names[i], app) || !strcmp(apps.names[i], app)) continue;
        clash = true;
        if (other) snprintf(other, cap, "%s", apps.names[i]);
    }
    free(apps.names);
    return clash;
}

int vfs_list(vfs_file_cb cb, void *ctx)
{
    name_set_t apps;
    int count = 0;
    collect_apps(&apps);
    for (int a = 0; a < apps.n; a++) {
        name_set_t files;
        collect_files(apps.names[a], &files);
        for (int i = 0; i < files.n; i++) {
            ram_file_t *f = ram_find(files.names[i]);   /* an upload shadows the installed file */
            size_t len;
            uint32_t crc;
            if (f) { len = f->len; crc = crc32_calc(f->data, f->len); }
            else if (!s_mounted || !flash_crc(files.names[i], &len, &crc)) continue;
            cb(files.names[i], len, crc, ctx);
            count++;
        }
        free(files.names);
    }
    free(apps.names);
    return count;
}

int vfs_apps(vfs_app_t *out, int max)
{
    name_set_t apps;
    collect_apps(&apps);
    int n = 0;
    for (int a = 0; a < apps.n && n < max; a++) {
        vfs_app_t *o = &out[n++];
        memset(o, 0, sizeof *o);
        snprintf(o->name, sizeof o->name, "%s", apps.names[a]);
        name_set_t files;
        collect_files(apps.names[a], &files);
        bool any_ram = false, any_flash = false;
        for (int i = 0; i < files.n; i++) {
            ram_file_t *f = ram_find(files.names[i]);
            long fs = s_mounted ? flash_size(files.names[i]) : -1;
            if (f) any_ram = true;
            if (fs >= 0) any_flash = true;
            o->files++;
            o->bytes += f ? f->len : (size_t)(fs > 0 ? fs : 0);
        }
        free(files.names);
        o->state = !any_ram ? VFS_INSTALLED : any_flash ? VFS_MODIFIED : VFS_RAM;
    }
    free(apps.names);
    return n;
}

int vfs_app_count(void)
{
    name_set_t apps;
    int n = collect_apps(&apps);
    free(apps.names);
    return n;
}

bool vfs_app_exists(const char *app)
{
    if (!vfs_valid_app(app)) return false;
    if (ram_has_app(app)) return true;
    char dir[256];
    struct stat st;
    flash_path(dir, sizeof dir, app);
    return s_mounted && stat(dir, &st) == 0 && S_ISDIR(st.st_mode);
}

static void count_path_cb(const char *path, void *ctx) { (*(int *)ctx)++; }

bool vfs_app_state(const char *app, vfs_app_state_t *state)
{
    if (!vfs_app_exists(app)) return false;
    int in_flash = 0;
    if (s_mounted) flash_files(app, count_path_cb, &in_flash);
    *state = !ram_has_app(app) ? VFS_INSTALLED : in_flash ? VFS_MODIFIED : VFS_RAM;
    return true;
}

/* ------------------------------------------------- install/uninstall */

esp_err_t vfs_install(const char *app)
{
    if (!vfs_valid_app(app)) return ESP_ERR_INVALID_ARG;
    if (!s_mounted) return ESP_ERR_INVALID_STATE;
    if (!vfs_app_exists(app)) return ESP_ERR_NOT_FOUND;
    esp_err_t err = ESP_OK;
    for (int i = 0; i < VFS_RAM_FILES && err == ESP_OK; i++) {
        ram_file_t *f = &s_ram[i];
        if (!f->data || !in_app(f->path, app)) continue;
        err = flash_store(f->path, f->data, f->len);
        if (err == ESP_OK) ram_drop(f);
    }
    changed();
    ESP_LOGI(TAG, "install %s: %s", app, esp_err_to_name(err));
    return err;
}

esp_err_t vfs_install_all(int *count)
{
    name_set_t apps;
    collect_apps(&apps);
    esp_err_t err = ESP_OK;
    int n = 0;
    for (int a = 0; a < apps.n && err == ESP_OK; a++) {
        if (!ram_has_app(apps.names[a])) continue;
        err = vfs_install(apps.names[a]);
        if (err == ESP_OK) n++;
    }
    free(apps.names);
    if (count) *count = n;
    return err;
}

static void remove_cb(const char *path, void *ctx) { (void)ctx; flash_remove(path); }

esp_err_t vfs_uninstall(const char *app)
{
    if (!vfs_app_exists(app)) return ESP_ERR_NOT_FOUND;
    for (int i = 0; i < VFS_RAM_FILES; i++)
        if (s_ram[i].data && in_app(s_ram[i].path, app)) ram_drop(&s_ram[i]);
    if (s_mounted) {
        char dir[256];
        flash_files(app, remove_cb, NULL);
        flash_path(dir, sizeof dir, app);
        /* leftovers of an interrupted write */
        DIR *d = opendir(dir);
        if (d) {
            struct dirent *e;
            while ((e = readdir(d))) {
                if (e->d_name[0] == '.') continue;
                char p[sizeof dir + 260];
                snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
                unlink(p);
            }
            closedir(d);
        }
        rmdir(dir);
    }
    changed();
    return ESP_OK;
}

esp_err_t vfs_format(void)
{
    for (int i = 0; i < VFS_RAM_FILES; i++) if (s_ram[i].data) ram_drop(&s_ram[i]);
    pend_drop();
    esp_err_t err = esp_littlefs_format(APPS_PARTITION);
    crc_forget_all();
    changed();
    return err;
}

void vfs_usage(size_t *ram, size_t *flash_used, size_t *flash_total)
{
    size_t total = 0, used = 0;
    if (s_mounted) esp_littlefs_info(APPS_PARTITION, &total, &used);
    if (ram) *ram = ram_used();
    if (flash_used) *flash_used = used;
    if (flash_total) *flash_total = total;
}

/* ------------------------------------------------------------- init */

void vfs_init(void)
{
    crc_forget_all();
    for (int i = 0; i < VFS_RAM_FILES; i++) if (s_ram[i].data) ram_drop(&s_ram[i]);
    esp_vfs_littlefs_conf_t conf = {
        .base_path = APPS_MOUNT,
        .partition_label = APPS_PARTITION,
        .format_if_mount_failed = 1,
    };
    esp_err_t err = esp_vfs_littlefs_register(&conf);
    s_mounted = err == ESP_OK;
    if (!s_mounted) {
        ESP_LOGE(TAG, "apps partition not mounted (%s): installing won't work", esp_err_to_name(err));
        return;
    }
    size_t total, used;
    esp_littlefs_info(APPS_PARTITION, &total, &used);
    ESP_LOGI(TAG, "apps partition: %zu of %zu KiB used", used / 1024, total / 1024);
    changed();
}
