/*
 * ESP32 mock platform - app slots (OTA), otadata and the bootloader.
 *
 * On the device an update is written into the app slot that isn't running,
 * the bootloader is told to boot it once (state NEW), and the new firmware
 * confirms itself (esp_ota_mark_app_valid_cancel_rollback). A slot that boots
 * and never confirms is rolled back at the next boot.
 *
 * Here an app image is a simulator executable for the same board, found by
 * the tag below (project, version, board). The slots are files in
 * "<nvs>.ota/": ota_0/ and ota_1/ hold an image each, next to a small text
 * otadata. At every boot the simulator plays the bootloader (sim_ota_boot):
 * it picks a slot the same way and replaces itself with that slot's image
 * (sim_main.c), so a reboot runs the new image for real: an update can be
 * tested end to end, rollback too.
 *
 * Launching an executable the slots don't know (a new build) counts as
 * flashing it over USB: it becomes ota_0 and otadata starts blank, as
 * `luaboard flash` does on the device.
 */
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "board.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "host.h"
#include "mock_internal.h"
#include "sim_hw.h"
#include "sim_version.h"        /* SIM_PROJECT_VER: git describe, from the build */

/* The firmware's name, as in the device image. */
#define SIM_PROJECT_NAME "luaboard"
#ifndef SIM_PROJECT_VER
#define SIM_PROJECT_VER "unknown"
#endif

/* The device's slots (app/idf/partitions.csv), but bigger: a simulator
   executable is no device image, and a debug or sanitizer build is bigger
   than the device's 3 MB. */
#define SLOT_SIZE (64 * 1024 * 1024)
static const esp_partition_t s_slots[2] = {
    { ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, 0x20000, SLOT_SIZE, "ota_0" },
    { ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, 0x320000, SLOT_SIZE, "ota_1" },
};

/* What makes an executable an app image, like esp_app_desc_t in a device
   image. For rollback tests, `fail=none` can be patched (same length) in a
   copy to `fail=init`: the image crashes where it confirms itself to the
   bootloader, or `fail=boot`: it crashes 1 s into every boot (codesign the
   copy again on macOS). */
#define TAG_MAX 160
__attribute__((used)) static const char k_tag[TAG_MAX] =
    "esp32-sim-image project=" SIM_PROJECT_NAME " ver=" SIM_PROJECT_VER " board=" BOARD_NAME " fail=none ";
/* Read through a volatile pointer: the compiler must not fold the tag, the
   bytes in the executable are what counts. */
static const char *volatile s_own_tag = k_tag;

typedef struct {
    char project[32], ver[32], board[32];
    char fail[8];                      /* none, init or boot */
} tag_t;

static char s_dir[600];
static char s_self[PATH_MAX];
static int s_running;

/* ------------------------------------------------------------------ tags */

static void tag_field(const char *tag, const char *key, char *out, size_t cap)
{
    *out = 0;
    char k[16];
    snprintf(k, sizeof k, " %s=", key);
    const char *p = strstr(tag, k);
    if (!p) return;
    p += strlen(k);
    size_t n = strcspn(p, " ");
    if (n >= cap) n = cap - 1;
    memcpy(out, p, n);
    out[n] = 0;
}

static bool tag_parse(const char *s, tag_t *t)
{
    char tag[TAG_MAX + 2] = " ";
    size_t n = strnlen(s, TAG_MAX);
    memcpy(tag + 1, s, n);
    tag[n + 1] = 0;
    tag_field(tag, "project", t->project, sizeof t->project);
    tag_field(tag, "ver", t->ver, sizeof t->ver);
    tag_field(tag, "board", t->board, sizeof t->board);
    tag_field(tag, "fail", t->fail, sizeof t->fail);
    return t->project[0] && t->board[0];
}

static void own_tag(tag_t *t) { tag_parse(s_own_tag, t); }

/* The tag of an executable file; false if it isn't an app image. */
static bool file_tag(const char *path, tag_t *t)
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    struct stat st;
    if (fstat(fileno(f), &st) != 0 || st.st_size < 64 || st.st_size > SLOT_SIZE) { fclose(f); return false; }
    unsigned char *buf = malloc((size_t)st.st_size);
    bool ok = buf && fread(buf, 1, (size_t)st.st_size, f) == (size_t)st.st_size;
    fclose(f);
    /* A program for this kind of computer (Mach-O, ELF, Windows .exe). */
    ok = ok && host_is_program(buf, (size_t)st.st_size);
    /* The needle is put together here, so it isn't a tag itself. */
    char needle[40];
    snprintf(needle, sizeof needle, "%s-sim-image project=", "esp32");
    bool found = false;
    for (unsigned char *p = buf; ok && !found;) {
        p = memmem(p, (size_t)(buf + st.st_size - p), needle, strlen(needle));
        if (!p) break;
        char tag[TAG_MAX + 1];
        size_t n = (size_t)(buf + st.st_size - p) < TAG_MAX ? (size_t)(buf + st.st_size - p) : TAG_MAX;
        memcpy(tag, p, n);
        tag[n] = 0;
        found = tag_parse(tag, t);
        p++;
    }
    free(buf);
    return ok && found;
}

/* ------------------------------------------------------------- otadata */

typedef struct {
    int boot;                          /* the slot to boot */
    esp_ota_img_states_t state[2];
    char flashed[128];                 /* the executable last flashed (file_id) */
} otadata_t;

static const char *state_name(esp_ota_img_states_t s)
{
    switch (s) {
    case ESP_OTA_IMG_NEW: return "new";
    case ESP_OTA_IMG_PENDING_VERIFY: return "pending";
    case ESP_OTA_IMG_VALID: return "valid";
    case ESP_OTA_IMG_INVALID: return "invalid";
    case ESP_OTA_IMG_ABORTED: return "aborted";
    default: return "undefined";
    }
}

static esp_ota_img_states_t state_parse(const char *s)
{
    for (int i = 0; i <= (int)ESP_OTA_IMG_ABORTED; i++)
        if (!strcmp(s, state_name((esp_ota_img_states_t)i))) return (esp_ota_img_states_t)i;
    return ESP_OTA_IMG_UNDEFINED;
}

static void od_path(char *out, size_t cap) { snprintf(out, cap, "%s/otadata", s_dir); }

static bool od_load(otadata_t *d)
{
    memset(d, 0, sizeof *d);
    d->state[0] = d->state[1] = ESP_OTA_IMG_UNDEFINED;
    char path[700], line[256];
    od_path(path, sizeof path);
    FILE *f = fopen(path, "r");
    if (!f) return false;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        const char *k = line, *v = eq + 1;
        if (!strcmp(k, "boot")) d->boot = atoi(v) == 1;
        else if (!strcmp(k, "state0")) d->state[0] = state_parse(v);
        else if (!strcmp(k, "state1")) d->state[1] = state_parse(v);
        else if (!strcmp(k, "flashed")) snprintf(d->flashed, sizeof d->flashed, "%s", v);
    }
    fclose(f);
    return true;
}

static void od_save(const otadata_t *d)
{
    char path[700], tmp[710];
    od_path(path, sizeof path);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) { fprintf(stderr, "bootloader: cannot write %s: %s\n", tmp, strerror(errno)); return; }
    fprintf(f, "boot=%d\nstate0=%s\nstate1=%s\nflashed=%s\n", d->boot, state_name(d->state[0]),
            state_name(d->state[1]), d->flashed);
    if (fclose(f) != 0 || rename(tmp, path) != 0) fprintf(stderr, "bootloader: cannot write %s\n", path);
}

/* --------------------------------------------------------------- slots */

/* "<dir>/<folder>/<this executable's name>": the folder is created as needed. */
static void image_path(const char *folder, char *out, size_t cap)
{
    const char *name = strrchr(s_self, '/');
#ifdef _WIN32
    const char *bs = strrchr(s_self, '\\');
    if (bs > name) name = bs;
#endif
    name = name ? name + 1 : s_self;
    snprintf(out, cap, "%s/%s", s_dir, folder);
    mkdir(out, 0755);
    size_t len = strlen(out);
    snprintf(out + len, cap - len, "/%s", *name ? name : "esp32-sim" HOST_EXE_SUFFIX);
}

static void slot_path(int n, char *out, size_t cap) { image_path(s_slots[n].label, out, cap); }

static int slot_index(const esp_partition_t *p)
{
    for (int i = 0; i < 2; i++) if (p == &s_slots[i] || (p && p->address == s_slots[i].address)) return i;
    return -1;
}

/* Holds a whole image: written to the end (executable bit) and tagged. */
static bool slot_ok(int n)
{
    char path[700];
    slot_path(n, path, sizeof path);
    tag_t t;
    return host_is_executable(path) && file_tag(path, &t);
}

void sim_ota_file_id(const char *path, char *out, size_t cap) { host_file_id(path, out, cap); }

/* Into a new file put in place of `to`: the process doing it may be running
   the old one, which must stay.

   With SIM_IMAGE_LINK set in the environment the new file is a symbolic
   link to `from`, no copy. The tests set it: macOS checks a program file it
   hasn't seen for 60 ms and more when it's first started, one after the
   other, and every test's store starts with such a copy. (Not a hard link:
   making one while another process starts the same file got that process
   killed by macOS now and then.) An update into the slot replaces the
   link, it doesn't write through it. Not for a simulator that is rebuilt
   while it runs (--watch): the slot would be the new build at once. */
static bool copy_file(const char *from, const char *to)
{
    char tmp[PATH_MAX + 8];
    snprintf(tmp, sizeof tmp, "%s.new", to);
#ifndef _WIN32
    if (getenv("SIM_IMAGE_LINK")) {
        char real[PATH_MAX];
        unlink(tmp);
        if (realpath(from, real) && symlink(real, tmp) == 0) {
            if (host_replace_file(tmp, to)) return true;
            unlink(tmp);
        }
    }
#else
    /* Windows: a hard link (a symbolic one needs a privilege). The virus
       scanner checks a program it hasn't seen when it's started: a copy per
       boot kept it busy for seconds on a test run's every simulator. */
    if (getenv("SIM_IMAGE_LINK")) {
        unlink(tmp);
        if (CreateHardLinkA(tmp, from, NULL)) {
            if (host_replace_file(tmp, to)) return true;
            unlink(tmp);
        }
    }
#endif
    FILE *in = fopen(from, "rb"), *out = in ? fopen(tmp, "wb") : NULL;
    bool ok = in && out;
    char buf[65536];
    size_t n;
    while (ok && (n = fread(buf, 1, sizeof buf, in)) > 0) ok = fwrite(buf, 1, n, out) == n;
    if (in) fclose(in);
    if (out && fclose(out) != 0) ok = false;
    ok = ok && host_set_executable(tmp, true) && host_replace_file(tmp, to);
    if (!ok) unlink(tmp);
    return ok;
}

static void self_init(void) { host_self_path(s_self, sizeof s_self); }

/* ---------------------------------------------------------- bootloader */

bool sim_ota_boot(const char *base, bool power_on, int *slot, char *image, size_t cap)
{
    snprintf(s_dir, sizeof s_dir, "%s.ota", base);
    mkdir(s_dir, 0755);
    self_init();

    otadata_t d;
    bool have = od_load(&d);
    char id[128], p0[700];
    sim_ota_file_id(s_self, id, sizeof id);
    slot_path(0, p0, sizeof p0);
    if (power_on && (!have || strcmp(d.flashed, id) != 0)) {
        /* Freshly flashed: this executable is ota_0, nothing else counts. */
        if (!copy_file(s_self, p0)) fprintf(stderr, "bootloader: cannot copy the image to %s\n", p0);
        memset(&d, 0, sizeof d);
        d.state[0] = d.state[1] = ESP_OTA_IMG_UNDEFINED;
        snprintf(d.flashed, sizeof d.flashed, "%s", id);
    }

    int b = d.boot;
    if (d.state[b] == ESP_OTA_IMG_NEW) {
        d.state[b] = ESP_OTA_IMG_PENDING_VERIFY;          /* its one try */
    } else if (d.state[b] == ESP_OTA_IMG_PENDING_VERIFY) {
        d.state[b] = ESP_OTA_IMG_ABORTED;                 /* it had its try and never confirmed */
        if (slot_ok(!b)) {
            fprintf(stderr, "bootloader: %s never confirmed itself, rolling back to %s\n", s_slots[b].label,
                    s_slots[!b].label);
            b = d.boot = !b;
        }
    }
    if (!slot_ok(b) && slot_ok(!b)) b = d.boot = !b;
    od_save(&d);
    *slot = s_running = b;
    slot_path(b, image, cap);
    return slot_ok(b);
}

bool sim_ota_image(const char *path, char *version, size_t cap)
{
    tag_t t;
    if (!file_tag(path, &t) || strcmp(t.board, BOARD_NAME) != 0) return false;
    snprintf(version, cap, "%s", t.ver);
    return true;
}

bool sim_ota_is_flashed(const char *path)
{
    otadata_t d;
    char id[128];
    sim_ota_file_id(path, id, sizeof id);
    return *id && od_load(&d) && !strcmp(d.flashed, id);
}

void sim_ota_chip(const char *base, int slot)
{
    snprintf(s_dir, sizeof s_dir, "%s.ota", base);
    self_init();
    s_running = slot == 1;
}

/* ------------------------------------------------------------- the API */

const esp_partition_t *esp_ota_get_running_partition(void) { return &s_slots[s_running]; }

const esp_partition_t *esp_ota_get_boot_partition(void)
{
    otadata_t d;
    od_load(&d);
    return &s_slots[d.boot];
}

const esp_partition_t *esp_ota_get_next_update_partition(const esp_partition_t *start_from)
{
    int i = slot_index(start_from ? start_from : &s_slots[s_running]);
    return &s_slots[i < 0 ? !s_running : !i];
}

static struct {
    FILE *f;
    int slot;
    size_t written;
} s_w = { .slot = -1 };

esp_err_t esp_ota_begin(const esp_partition_t *partition, size_t image_size, esp_ota_handle_t *out_handle)
{
    int i = slot_index(partition);
    if (i < 0 || !out_handle) return ESP_ERR_INVALID_ARG;
    if (i == s_running) return ESP_ERR_OTA_PARTITION_CONFLICT;
    if (image_size != OTA_SIZE_UNKNOWN && image_size != OTA_WITH_SEQUENTIAL_WRITES && image_size > SLOT_SIZE)
        return ESP_ERR_INVALID_SIZE;
    if (s_w.f) fclose(s_w.f);
    char path[700];
    slot_path(i, path, sizeof path);
    /* Erasing the slot: its old image is gone, even if this write never ends. */
    unlink(path);
    s_w.f = fopen(path, "wb");
    if (!s_w.f) return ESP_FAIL;
    host_set_executable(path, false);
    s_w.slot = i;
    s_w.written = 0;
    *out_handle = 1;
    return ESP_OK;
}

esp_err_t esp_ota_write(esp_ota_handle_t handle, const void *data, size_t size)
{
    if (handle != 1 || !s_w.f) return ESP_ERR_INVALID_ARG;
    if (s_w.written + size > SLOT_SIZE) return ESP_ERR_INVALID_SIZE;
    if (fwrite(data, 1, size, s_w.f) != size) return ESP_FAIL;
    s_w.written += size;
    return ESP_OK;
}

esp_err_t esp_ota_abort(esp_ota_handle_t handle)
{
    if (handle != 1 || !s_w.f) return ESP_ERR_INVALID_ARG;
    fclose(s_w.f);
    s_w.f = NULL;
    s_w.slot = -1;
    return ESP_OK;
}

esp_err_t esp_ota_end(esp_ota_handle_t handle)
{
    if (handle != 1 || !s_w.f) return ESP_ERR_INVALID_ARG;
    int i = s_w.slot;
    bool ok = fclose(s_w.f) == 0;
    s_w.f = NULL;
    s_w.slot = -1;
    char path[700];
    slot_path(i, path, sizeof path);
    tag_t t;
    /* Like the device's image check: an app image for this chip (board). */
    if (!ok || !file_tag(path, &t) || strcmp(t.board, BOARD_NAME) != 0)
        return ESP_ERR_OTA_VALIDATE_FAILED;
    return host_set_executable(path, true) ? ESP_OK : ESP_FAIL;
}

esp_err_t esp_ota_set_boot_partition(const esp_partition_t *partition)
{
    int i = slot_index(partition);
    if (i < 0) return ESP_ERR_INVALID_ARG;
    if (!slot_ok(i)) return ESP_ERR_OTA_VALIDATE_FAILED;
    otadata_t d;
    od_load(&d);
    d.boot = i;
    if (i != s_running) d.state[i] = ESP_OTA_IMG_NEW;
    od_save(&d);
    return ESP_OK;
}

/* app_elf_sha256 stands for the build: here a hash of the executable (FNV-1a,
   in the first 8 bytes), so two builds of one version differ, as on the
   device. */
static void file_hash(const char *path, uint8_t out[32])
{
    uint64_t h = 0xcbf29ce484222325ull;
    FILE *f = fopen(path, "rb");
    unsigned char buf[4096];                          /* (on the firmware's stack) */
    size_t n;
    while (f && (n = fread(buf, 1, sizeof buf, f)) > 0)
        for (size_t i = 0; i < n; i++) h = (h ^ buf[i]) * 0x100000001b3ull;
    if (f) fclose(f);
    for (int i = 0; i < 8; i++) out[i] = (uint8_t)(h >> (56 - 8 * i));
}

static void desc_from_tag(const tag_t *t, const char *path, esp_app_desc_t *desc)
{
    memset(desc, 0, sizeof *desc);
    file_hash(path, desc->app_elf_sha256);
    desc->magic_word = 0xABCD5432;
    snprintf(desc->version, sizeof desc->version, "%s", t->ver);
    snprintf(desc->project_name, sizeof desc->project_name, "%s", t->project);
    snprintf(desc->idf_ver, sizeof desc->idf_ver, "simulator");
}

esp_err_t esp_ota_get_partition_description(const esp_partition_t *partition, esp_app_desc_t *app_desc)
{
    int i = slot_index(partition);
    if (i < 0 || !app_desc) return ESP_ERR_INVALID_ARG;
    if (i == s_running) {
        *app_desc = *esp_app_get_description();
        return ESP_OK;
    }
    tag_t t;
    char path[700];
    slot_path(i, path, sizeof path);
    if (!file_tag(path, &t)) return ESP_ERR_NOT_FOUND;
    desc_from_tag(&t, path, app_desc);
    return ESP_OK;
}

esp_err_t esp_ota_get_state_partition(const esp_partition_t *partition, esp_ota_img_states_t *ota_state)
{
    int i = slot_index(partition);
    if (i < 0 || !ota_state) return ESP_ERR_INVALID_ARG;
    otadata_t d;
    od_load(&d);
    *ota_state = d.state[i];
    return ESP_OK;
}

esp_err_t esp_ota_mark_app_valid_cancel_rollback(void)
{
    tag_t t;
    own_tag(&t);
    if (!strcmp(t.fail, "init")) {
        /* An image made to fail: it crashes where it would confirm itself,
           and the chip resets (the bootloader then rolls it back). */
        fprintf(stderr, "simulated crash: this image (fail=init) crashes before it confirms itself\n");
        mock_restart(ESP_RST_PANIC);
    }
    otadata_t d;
    od_load(&d);
    if (d.state[s_running] != ESP_OTA_IMG_VALID) {
        d.state[s_running] = ESP_OTA_IMG_VALID;
        od_save(&d);
    }
    return ESP_OK;
}

bool mock_ota_fail_boot(void)
{
    static int fail = -1;
    if (fail < 0) {
        tag_t t;
        own_tag(&t);
        fail = !strcmp(t.fail, "boot");
    }
    return fail;
}

static esp_app_desc_t s_desc;
/* --set fw_version=: the version the firmware sees, "" for the build's. */
static char s_version_override[32];

void sim_ota_set_version(const char *ver)
{
    snprintf(s_version_override, sizeof s_version_override, "%s", ver ? ver : "");
    s_desc.magic_word = 0;                            /* made again with it */
}

const esp_app_desc_t *esp_app_get_description(void)
{
    if (!s_desc.magic_word) {
        tag_t t;
        own_tag(&t);
        desc_from_tag(&t, s_self, &s_desc);
        if (s_version_override[0]) snprintf(s_desc.version, sizeof s_desc.version, "%s", s_version_override);
    }
    return &s_desc;
}
