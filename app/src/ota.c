#include "ota.h"

#include <stdio.h>
#include <string.h>

#include "app_config.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "hex.h"
#include "hw.h"
#include "menu.h"
#include "net.h"
#include "protocol.h"
#include "screens.h"
#include "script.h"
#include "settings.h"
#include "sha256.h"

static const char *TAG = "ota";

static struct {
    bool busy;
    esp_ota_handle_t handle;
    const esp_partition_t *part;
    size_t size, got;
    lb_sha256_ctx_t sha;
    uint8_t want[32];
    esp_err_t err;
    int pct;                       /* shown on the screen */
    char resume[64];               /* the script it stopped, started again if it fails */
} s_up;

static const char *s_state = "ok";
static bool s_updated;             /* this is the first boot of a new update */
static bool s_watching;            /* an update not proven yet: crash boots count */
static uint32_t s_reboot_at;       /* hw_millis() to reboot at, 0: none */
static bool s_dropped;             /* the clients were told: the reboot follows */

const char *fw_version(void) { return esp_app_get_description()->version; }

/* A build, as hex text: the start of the image's ELF hash, which differs
   between two builds that share a version (a -dirty one). */
static void fw_id(const esp_app_desc_t *d, char out[17])
{
    hex_encode(d->app_elf_sha256, 8, out);
}

/* An update until it's proven (NVS_KEY_OTA): written by the firmware that
   installed it, then kept by the new one through its crash boots. It's
   proven by running OTA_STABLE_MS. Stored as text, "<slot> <booted>
   <crashes> <id> <from>", so that firmware with a different idea of this
   struct still reads it. The id tells the update from another firmware put
   in the same slot later (over USB), which isn't watched. */
typedef struct {
    char slot[17];                 /* where it went (a partition label) */
    char id[17];                   /* which build (fw_id) */
    char from[32];                 /* the version it replaced */
    int booted;                    /* it has booted once */
    int crashes;                   /* boots in a row since then that ended in a crash */
} ota_rec_t;

static bool rec_get(ota_rec_t *r)
{
    char text[100];
    memset(r, 0, sizeof *r);
    text[settings_load(NVS_KEY_OTA, text, sizeof text - 1)] = 0;
    return sscanf(text, "%16s %d %d %16s %31[^\n]", r->slot, &r->booted, &r->crashes, r->id, r->from) >= 4;
}

/* Store r, or with NULL forget the update; `failed`: also note that the
   running version failed, for the firmware that boots next (NVS_KEY_OTA_FAILED).
   False if it couldn't be stored. */
static bool rec_set(const ota_rec_t *r, const char *failed)
{
    char text[100] = "";
    if (r) snprintf(text, sizeof text, "%s %d %d %s %s", r->slot, r->booted, r->crashes, r->id, r->from);
    esp_err_t e = settings_store(NVS_KEY_OTA, text, strlen(text));
    if (e == ESP_OK && failed) e = settings_store(NVS_KEY_OTA_FAILED, failed, strlen(failed));
    if (e != ESP_OK) ESP_LOGE(TAG, "cannot store the update's state: %s", esp_err_to_name(e));
    return e == ESP_OK;
}

/* An update that failed and was rolled back by this firmware's predecessor
   (taken once: true the first time it's asked). */
static bool failed_take(void)
{
    char v[32];
    return settings_take(NVS_KEY_OTA_FAILED, v, sizeof v) > 0;
}

const char *ota_slot(void)
{
    const esp_partition_t *p = esp_ota_get_running_partition();
    return p ? p->label : "-";
}

const char *ota_state(void) { return s_state; }
bool ota_just_updated(void) { return s_updated; }

/* Boot the other slot's firmware (the one before the update) instead. */
static void roll_back(const ota_rec_t *r)
{
    const esp_partition_t *other = esp_ota_get_next_update_partition(NULL);
    esp_app_desc_t desc;
    if (!other || esp_ota_get_partition_description(other, &desc) != ESP_OK || strcmp(desc.project_name, FW_NAME) != 0 ||
        esp_ota_set_boot_partition(other) != ESP_OK) {
        ESP_LOGE(TAG, "%s keeps crashing, but there's no firmware to go back to", fw_version());
        rec_set(NULL, NULL);
        return;
    }
    ESP_LOGW(TAG, "%s crashed %d boots in a row: back to %s", fw_version(), r->crashes, desc.version);
    rec_set(NULL, fw_version());
    esp_restart();
}

void ota_boot(void)
{
    /* The bootloader rolls back a new firmware that resets before this point
       (state PENDING_VERIFY); from here on the boot-loop count below does. */
    esp_ota_img_states_t st;
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (esp_ota_get_state_partition(running, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY)
        esp_ota_mark_app_valid_cancel_rollback();

    if (failed_take()) s_state = "rolledback";
    ota_rec_t r;
    if (!rec_get(&r)) return;
    char id[17];
    fw_id(esp_app_get_description(), id);
    if (strcmp(r.slot, running->label) != 0 || strcmp(r.id, id) != 0) {
        /* This isn't the update. If the bootloader marked its slot aborted,
           it crashed before the point above; otherwise this was put here
           some other way (flashed over USB). */
        const esp_partition_t *other = esp_ota_get_next_update_partition(NULL);
        esp_ota_img_states_t ost;
        if (other && !strcmp(other->label, r.slot) && esp_ota_get_state_partition(other, &ost) == ESP_OK &&
            (ost == ESP_OTA_IMG_ABORTED || ost == ESP_OTA_IMG_INVALID)) {
            s_state = "rolledback";
            ESP_LOGW(TAG, "the update crashed at boot: back to %s", fw_version());
        }
        rec_set(NULL, NULL);
        return;
    }
    if (!r.booted) {                                  /* its first boot */
        r.booted = 1;
        rec_set(&r, NULL);
        s_updated = s_watching = true;
        ESP_LOGI(TAG, "updated from %s to %s", r.from, fw_version());
        return;
    }
    s_watching = true;
    if (!hw_reset_was_fw_crash()) {
        /* Not a crash, yet not proof either: a power cycle may be someone
           ending a hang. The streak starts over; a hang the watchdog ends
           counts as a crash. */
        if (r.crashes) { r.crashes = 0; rec_set(&r, NULL); }
        return;
    }
    r.crashes++;
    if (r.crashes >= OTA_ROLLBACK_BOOTS) roll_back(&r);   /* (returns if it can't) */
    else {
        rec_set(&r, NULL);
        ESP_LOGW(TAG, "%s crashed (%d of %d boots before going back)", fw_version(), r.crashes, OTA_ROLLBACK_BOOTS);
    }
}

void ota_poll(void)
{
    if (s_reboot_at && (int32_t)(hw_millis() - s_reboot_at) >= 0) {
        if (!s_dropped) {
            /* A reset doesn't close TCP connections: a PC on Wi-Fi would
               wait on a dead one. Close them, so they reconnect to the new
               firmware, and give the closing a moment to go out. */
            net_drop_clients("restarting into the new firmware");
            s_dropped = true;
            s_reboot_at = (hw_millis() + 200) | 1;
            return;
        }
        ESP_LOGI(TAG, "rebooting into the new firmware");
        esp_restart();
    }
    if (s_watching && hw_millis() >= OTA_STABLE_MS) {  /* ran long enough: proven */
        s_watching = false;
        rec_set(NULL, NULL);
    }
}

bool ota_active(void) { return s_up.busy || s_reboot_at; }
bool ota_restarting(void) { return s_dropped; }

static void resume(void);

static void show_progress(void)
{
    int pct = s_up.size ? (int)(s_up.got * 100 / s_up.size) : 0;
    if (pct == s_up.pct) return;
    s_up.pct = pct;
    char line[64];
    snprintf(line, sizeof line, "%zu / %zu KB", s_up.got / 1024, s_up.size / 1024);
    screen_update("Updating firmware", line, pct);
}

bool ota_begin(size_t size, const uint8_t sha[32], char *err, size_t cap)
{
    if (ota_active()) { snprintf(err, cap, "an update is already running"); return false; }
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) { snprintf(err, cap, "no app slot to update"); return false; }
    if (size == 0 || size > part->size) {
        snprintf(err, cap, "image size %zu out of range (the slot holds %u)", size, (unsigned)part->size);
        return false;
    }
    snprintf(s_up.resume, sizeof s_up.resume, "%s", script_current());
    if (script_running()) script_stop();
    screen_update("Updating firmware", "starting", 0);
    esp_err_t e = esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &s_up.handle);
    if (e != ESP_OK) {
        snprintf(err, cap, "cannot start writing %s: %s", part->label, esp_err_to_name(e));
        resume();
        return false;
    }
    s_up.busy = true;
    s_up.part = part;
    s_up.size = size;
    s_up.got = 0;
    s_up.err = ESP_OK;
    s_up.pct = -1;
    memcpy(s_up.want, sha, 32);
    lb_sha256_init(&s_up.sha);
    proto_eventf("ota started size=%zu slot=%s", size, part->label);
    show_progress();
    return true;
}

size_t ota_write(const void *data, size_t len)
{
    if (!s_up.busy) return 0;
    if (len > s_up.size - s_up.got) len = s_up.size - s_up.got;
    lb_sha256_update(&s_up.sha, data, len);
    if (s_up.err == ESP_OK) s_up.err = esp_ota_write(s_up.handle, data, len);
    s_up.got += len;
    show_progress();
    return len;
}

size_t ota_received(size_t *size)
{
    *size = s_up.size;
    return s_up.got;
}

/* Back to what ran before the update: the script it stopped, or the menu. */
static void resume(void)
{
    char err[200];
    if (s_up.resume[0] && script_run(s_up.resume, err, sizeof err)) proto_eventf("started %s", script_current());
    else if (!s_up.resume[0]) menu_show();      /* (a script that fails shows its error) */
}

static void failed(const char *why)
{
    char msg[80];
    snprintf(msg, sizeof msg, "update failed: %s", why);
    proto_eventf("ota failed %s", why);
    resume();
    menu_toast(msg);
}

void ota_abort(const char *why)
{
    if (!s_up.busy) return;
    esp_ota_abort(s_up.handle);
    s_up.busy = false;
    failed(why);
}

bool ota_finish(char *msg, size_t cap)
{
    if (!s_up.busy) { snprintf(msg, cap, "no update running"); return false; }
    s_up.busy = false;
    uint8_t got[32];
    lb_sha256_final(&s_up.sha, got);
    const char *why = NULL;
    esp_app_desc_t desc = { 0 };
    if (s_up.err != ESP_OK) {
        snprintf(msg, cap, "writing the slot failed: %s", esp_err_to_name(s_up.err));
        why = "write error";
    } else if (memcmp(got, s_up.want, 32) != 0) {
        snprintf(msg, cap, "sha256 mismatch: the image arrived damaged");
        why = "damaged image";
    }
    if (why) {
        esp_ota_abort(s_up.handle);
        failed(why);
        return false;
    }
    screen_update("Updating firmware", "checking the image", 100);
    esp_err_t e = esp_ota_end(s_up.handle);
    if (e != ESP_OK) {
        snprintf(msg, cap, "not a firmware image for this device (%s)", esp_err_to_name(e));
        failed("not a firmware image for this device");
        return false;
    }
    if (esp_ota_get_partition_description(s_up.part, &desc) != ESP_OK || strcmp(desc.project_name, FW_NAME) != 0) {
        snprintf(msg, cap, "not %s firmware (project '%s')", FW_NAME, desc.project_name);
        failed("not " FW_NAME " firmware");
        return false;
    }
    /* First the record, so the new firmware is watched from its first boot;
       then the switch, which puts the old record back if it fails. */
    ota_rec_t prev, r = { .booted = 0 };
    bool had = s_watching && rec_get(&prev);          /* this firmware's own, unproven yet */
    snprintf(r.slot, sizeof r.slot, "%s", s_up.part->label);
    fw_id(&desc, r.id);
    snprintf(r.from, sizeof r.from, "%s", fw_version());
    if (!rec_set(&r, NULL)) {
        snprintf(msg, cap, "cannot store the update's state (NVS)");
        failed("cannot store the update's state");
        return false;
    }
    e = esp_ota_set_boot_partition(s_up.part);
    if (e != ESP_OK) {
        rec_set(had ? &prev : NULL, NULL);
        snprintf(msg, cap, "cannot boot %s: %s", s_up.part->label, esp_err_to_name(e));
        failed("cannot boot the new image");
        return false;
    }
    s_watching = false;                               /* the next boot is the new one's */
    char line[64];
    snprintf(line, sizeof line, "restarting into %.40s", desc.version);
    screen_update("Firmware updated", line, 100);
    proto_eventf("ota installed ver=%s slot=%s rebooting", desc.version, s_up.part->label);
    snprintf(msg, cap, "%s", desc.version);
    s_reboot_at = (hw_millis() + OTA_REBOOT_DELAY_MS) | 1;   /* the reply gets out first */
    return true;
}
