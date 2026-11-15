/* Apps from the marketplace. See app_update.h. */
#include "app_update.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_config.h"
#include "appmeta.h"
#include "check_sched.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "httpc.h"
#include "hw.h"
#include "net.h"
#include "ota.h"
#include "protocol.h"
#include "semver.h"
#include "script.h"
#include "settings.h"
#include "sha256.h"
#include "vfs.h"

#ifndef ESP_PLATFORM
/* The simulator's options (--set apps_url=, update_every=; sim_hw.h). */
const char *sim_apps_url(void);
uint32_t sim_update_every(void);
#endif

_Static_assert(APPS_NAME_MAX == VFS_APP_NAME_MAX, "index names are app names");
_Static_assert(APPS_PATH_MAX == VFS_FILE_NAME_MAX, "index paths are file names");
_Static_assert(APPS_FILE_MAX == VFS_FILE_MAX, "index files are files the board loads");

#define RECHECK_MS (60 * 60 * 1000)     /* "Get apps" checks again when the index is older */

static const char *TAG = "apps";

static httpc_t *s_http;                 /* a check in flight */
static char *s_body;                    /* its answer so far (APPS_INDEX_MAX) */
static size_t s_len;
static bool s_too_big;
static char *s_text;                    /* the index in use: s_ix's entries point into it */
static size_t s_text_len;
static apps_index_t *s_ix, *s_next;     /* in use; the next one, parsed before they swap */
static bool s_have;
static uint32_t s_checked_at;           /* hw_millis of the last check that worked */
static check_sched_t s_sched;
static char s_err[48];

static uint32_t s_gen;                  /* vfs_generation() when the apps were last compared */
static bool s_compared;
static const apps_entry_t *s_upd[VFS_MAX_APPS];
static int s_nupd;
static int s_told;                      /* the count "app updates" last went out with (0 at start: no event for none) */

/* An install (app_install_start), stepped from app_update_poll. */
static struct {
    bool busy;
    apps_entry_t e;                     /* a copy: a check may replace the index meanwhile */
    apps_file_t files[APPS_INDEX_MAX_FILES];
    int n, i;                           /* files, and the one being fetched */
    bool keep_ok;                       /* no uploads in RAM: installed files can be kept */
    httpc_t *dl;
    uint8_t *buf;                       /* the file so far */
    size_t len;
    bool too_big;
    const char *result;                 /* NULL: it worked */
    char why[HTTPC_SHORT_MAX + 1];
} s_in;

static const char *base_url(void)
{
#ifdef ESP_PLATFORM
    return APPS_URL;
#else
    return sim_apps_url();
#endif
}

static uint32_t check_every(void)
{
#ifdef ESP_PLATFORM
    return UPDATE_CHECK_EVERY_MS;
#else
    uint32_t ms = sim_update_every();
    return ms ? ms : UPDATE_CHECK_EVERY_MS;
#endif
}

static bool wifi_up(void) { return strcmp(net_ip(), "-") != 0; }

/* In PSRAM where there is some (the index is tens of KB). */
static void *big_alloc(size_t n)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : malloc(n);
}

static bool have_buffers(void)
{
    if (!s_body) s_body = big_alloc(APPS_INDEX_MAX);
    if (!s_text) s_text = big_alloc(APPS_INDEX_MAX);
    if (!s_ix) s_ix = big_alloc(sizeof *s_ix);
    if (!s_next) s_next = big_alloc(sizeof *s_next);
    return s_body && s_text && s_ix && s_next;
}

/* Which installed apps have an update. */
static void compare(void)
{
    static vfs_app_t apps[VFS_MAX_APPS];
    s_gen = vfs_generation();
    s_compared = true;
    s_nupd = 0;
    if (s_have) {
        int n = vfs_apps(apps, VFS_MAX_APPS);
        for (int i = 0; i < n && s_nupd < VFS_MAX_APPS; i++) {
            const apps_entry_t *e = apps_index_find(s_ix, apps[i].name);
            if (!e || apps[i].state != VFS_INSTALLED) continue;   /* uploads in RAM: being worked on */
            app_meta_t m;
            app_meta_load(apps[i].name, &m);          /* no version: older than any */
            if (semver_cmp(e->version, m.version) > 0) s_upd[s_nupd++] = e;
        }
    }
    if (s_nupd != s_told) {
        s_told = s_nupd;
        proto_eventf("app updates n=%d", s_nupd);
    }
}

static bool on_body(void *ctx, const uint8_t *data, size_t len)
{
    (void)ctx;
    if (len > APPS_INDEX_MAX - s_len) {
        s_too_big = true;
        return false;
    }
    memcpy(s_body + s_len, data, len);
    s_len += len;
    return true;
}

static void check_over(bool ok)
{
    httpc_free(s_http);
    s_http = NULL;
    check_sched_done(&s_sched, hw_millis(), ok, check_every());
}

/* What was known before stays. */
static void check_failed(const char *why)
{
    ESP_LOGW(TAG, "check failed: %s", why);
    snprintf(s_err, sizeof s_err, "%s", why);
    proto_eventf("apps check-failed %s", why);
    check_over(false);
}

static void check_done(void)
{
    char err[48];
    if (!apps_index_parse(s_body, s_len, APP_LUA_API_MIN, APP_LUA_API, s_next, err, sizeof err)) {
        char why[64];
        snprintf(why, sizeof why, "index: %s", err);
        check_failed(why);
        return;
    }
    apps_index_t *ix = s_next;                       /* the new one goes in use */
    s_next = s_ix;
    s_ix = ix;
    char *text = s_body;
    s_body = s_text;
    s_text = text;
    s_text_len = s_len;
    s_have = true;
    s_err[0] = 0;
    s_checked_at = hw_millis();
    check_over(true);
    compare();
    if (s_ix->newer_format)
        ESP_LOGW(TAG, "check done (0 apps, 0 updates): the index is for a later firmware");
    else
        ESP_LOGI(TAG, "check done (%d apps, %d updates)", s_ix->n, s_nupd);
}

static void check_start(void)
{
    const char *base = base_url();
    char url[600], err[80];
    if (!base) return;
    if (strncmp(base, "https://", 8)) {
        check_failed("the marketplace address isn't https");
        return;
    }
    if (!have_buffers()) {
        check_failed("out of memory");
        return;
    }
    snprintf(url, sizeof url, "%s/main/index.json", base);
    s_len = 0;
    s_too_big = false;
    s_http = httpc_get(url, on_body, NULL, err, sizeof err);
    if (!s_http) check_failed(err);
}

/* ------------------------------------------------------------ installing */

static void install_over(const char *why)
{
    httpc_free(s_in.dl);
    s_in.dl = NULL;
    free(s_in.buf);
    s_in.buf = NULL;
    s_in.busy = false;
    const char *app = s_in.e.name;
    if (why) {
        vfs_stage_abort(app);
        snprintf(s_in.why, sizeof s_in.why, "%s", why);
        s_in.result = s_in.why;
        ESP_LOGW(TAG, "install %s: failed: %s", app, why);
        proto_eventf("app install %s failed %s", app, why);
    } else {
        s_in.result = NULL;
        ESP_LOGI(TAG, "install %s: ok", app);
        proto_eventf("app install %s ok", app);
    }
}

static bool on_file(void *ctx, const uint8_t *data, size_t len)
{
    (void)ctx;
    if (len > s_in.files[s_in.i].size - s_in.len) {
        s_in.too_big = true;
        return false;
    }
    memcpy(s_in.buf + s_in.len, data, len);
    s_in.len += len;
    return true;
}

/* Whether the installed file is already the one wanted. */
static bool same_as_installed(const apps_file_t *f)
{
    if (!s_in.keep_ok) return false;
    char path[VFS_MAX_NAME];
    snprintf(path, sizeof path, "%s/%s", s_in.e.name, f->path);
    size_t len;
    uint8_t *data = vfs_load(path, &len), sha[32];
    if (!data) return false;
    bool same = len == f->size;
    if (same) {
        lb_sha256(data, len, sha);
        same = !memcmp(sha, f->sha256, sizeof sha);
    }
    free(data);
    return same;
}

/* The file s_in.i: kept, or its download started. */
static void next_file(void)
{
    for (; s_in.i < s_in.n; s_in.i++) {
        const apps_file_t *f = &s_in.files[s_in.i];
        if (!same_as_installed(f)) break;
        if (vfs_stage_keep(s_in.e.name, f->path) != ESP_OK) { install_over("no room"); return; }
    }
    if (s_in.i == s_in.n) {
        install_over(vfs_stage_commit(s_in.e.name) == ESP_OK ? NULL : "no room");
        return;
    }
    const apps_file_t *f = &s_in.files[s_in.i];
    char url[700], err[80];
    snprintf(url, sizeof url, "%s/%s/%s/%s", base_url(), s_in.e.commit, s_in.e.name, f->path);
    s_in.buf = big_alloc(f->size ? f->size : 1);
    if (!s_in.buf) { install_over("out of memory"); return; }
    s_in.len = 0;
    s_in.too_big = false;
    s_in.dl = httpc_get(url, on_file, NULL, err, sizeof err);
    if (!s_in.dl) {
        char buf[HTTPC_SHORT_MAX + 1];
        install_over(httpc_short_error(err, buf, sizeof buf));
    }
}

static void install_poll(void)
{
    httpc_status_t st = httpc_poll(s_in.dl);
    if (st == HTTPC_BUSY) return;
    const apps_file_t *f = &s_in.files[s_in.i];
    if (st == HTTPC_FAILED && !s_in.too_big) {
        char buf[HTTPC_SHORT_MAX + 1];
        install_over(httpc_short_error(httpc_error(s_in.dl), buf, sizeof buf));
        return;
    }
    uint8_t sha[32];
    lb_sha256(s_in.buf, s_in.len, sha);
    if (s_in.too_big || s_in.len != f->size || memcmp(sha, f->sha256, sizeof sha)) {
        install_over("file changed on the way");
        return;
    }
    esp_err_t err = vfs_stage_write(s_in.e.name, f->path, s_in.buf, s_in.len);
    httpc_free(s_in.dl);
    s_in.dl = NULL;
    free(s_in.buf);
    s_in.buf = NULL;
    if (err != ESP_OK) { install_over("no room"); return; }
    s_in.i++;
    next_file();
}

bool app_install_start(const char *app, char *err, size_t cap)
{
    const apps_entry_t *e = s_have ? apps_index_find(s_ix, app) : NULL;
    const char *running = script_running() ? script_current() : "";
    size_t n = strlen(app);
    if (s_in.busy) { snprintf(err, cap, "an install is under way"); return false; }
    if (!e) { snprintf(err, cap, s_have ? "not in the marketplace" : "the marketplace isn't known yet"); return false; }
    if (!strncmp(running, app, n) && running[n] == '/') { snprintf(err, cap, "the app is running"); return false; }
    if (!wifi_up()) { snprintf(err, cap, "no Wi-Fi"); return false; }
    memset(&s_in, 0, sizeof s_in);
    s_in.e = *e;
    s_in.n = apps_index_files(s_text, s_text_len, e, s_in.files, APPS_INDEX_MAX_FILES);
    if (s_in.n < 0) { snprintf(err, cap, "the index lists its files wrongly"); return false; }
    vfs_app_state_t state;
    s_in.keep_ok = vfs_app_state(app, &state) && state == VFS_INSTALLED;
    if (vfs_stage_begin(app) != ESP_OK) { snprintf(err, cap, "no room"); return false; }
    s_in.busy = true;
    ESP_LOGI(TAG, "installing %s %s (%d files)", app, e->version, s_in.n);
    next_file();
    return true;
}

bool app_install_busy(void) { return s_in.busy; }

const char *app_install_app(void) { return s_in.e.name; }

int app_install_pct(void)
{
    if (!s_in.busy) return 100;
    if (!s_in.n) return 0;
    const apps_file_t *f = &s_in.files[s_in.i < s_in.n ? s_in.i : s_in.n - 1];
    int part = f->size ? (int)(s_in.len * 100 / f->size) : 0;
    return (s_in.i * 100 + part) / s_in.n;
}

const char *app_install_file(void) { return s_in.i < s_in.n ? s_in.files[s_in.i].path : ""; }

const char *app_install_result(void) { return s_in.result; }

/* ------------------------------------------------------------ the check */

void app_update_init(void)
{
    const char *base = base_url();
    if (base) ESP_LOGI(TAG, "marketplace at %s", base);
    else ESP_LOGI(TAG, "no marketplace address: no app checks");
}

void app_update_poll(void)
{
    if (s_in.dl) install_poll();
    if (s_http) {
        switch (httpc_poll(s_http)) {
        case HTTPC_BUSY: return;
        case HTTPC_DONE: check_done(); return;
        case HTTPC_FAILED: check_failed(s_too_big ? "index too big" : httpc_error(s_http)); return;
        }
    }
    if (!s_compared || s_gen != vfs_generation()) compare();
    if (!settings_update_check() || !base_url() || !wifi_up()) return;
    if (!check_sched_due(&s_sched, hw_millis(), UPDATE_FIRST_CHECK_MS) || ota_active() || s_in.busy) return;
    check_start();
}

void app_update_setting_changed(void)
{
    if (!settings_update_check()) {
        httpc_free(s_http);
        s_http = NULL;
        s_have = false;
        check_sched_reset(&s_sched);
        compare();
        return;
    }
    if (s_http) return;
    check_sched_reset(&s_sched);
    if (wifi_up()) check_sched_soon(&s_sched, hw_millis(), UPDATE_FIRST_CHECK_MS);
}

void app_update_check_now(void)
{
    if (s_have && hw_millis() - s_checked_at < RECHECK_MS) return;
    app_update_check_force();
}

void app_update_check_force(void)
{
    if (s_http || !base_url() || !wifi_up() || ota_active() || s_in.busy) return;
    check_start();
}

bool app_update_checking(void) { return s_http != NULL; }

bool app_update_configured(void) { return base_url() != NULL; }

const apps_index_t *app_update_index(void) { return s_have ? s_ix : NULL; }

const char *app_update_text(size_t *len)
{
    if (len) *len = s_have ? s_text_len : 0;
    return s_have ? s_text : NULL;
}

const char *app_update_error(void) { return s_err; }

int app_update_count(void) { return s_nupd; }

const apps_entry_t *app_update_at(int i) { return i >= 0 && i < s_nupd ? s_upd[i] : NULL; }

const apps_entry_t *app_update_for(const char *app)
{
    for (int i = 0; i < s_nupd; i++)
        if (!strcmp(s_upd[i]->name, app)) return s_upd[i];
    return NULL;
}
