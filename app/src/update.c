/* The update check. See update.h. */
#include "update.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_config.h"
#include "check_sched.h"
#include "board.h"
#include "esp_log.h"
#include "httpc.h"
#include "hw.h"
#include "net.h"
#include "ota.h"
#include "protocol.h"
#include "settings.h"

#ifndef ESP_PLATFORM
/* The simulator's options (--set update_url=, update_every=; declared in
   sim_hw.h, which firmware code doesn't include). */
const char *sim_update_url(void);
uint32_t sim_update_every(void);
#endif

static const char *TAG = "update";

static httpc_t *s_http;                 /* a check in flight */
static char s_body[UPDATE_MANIFEST_MAX];
static size_t s_len;
static bool s_too_big;
static update_manifest_t s_release;     /* the newer release, when s_have */
static bool s_have;
static bool s_manual;                   /* the release's manifest is in a later format: see update_manual */
static char s_told[32];                 /* the version "update available" went out for last */
static check_sched_t s_sched;           /* when the next check runs */
static char s_err[48];                  /* why the last check failed, "" if it didn't */
static httpc_t *s_dl;                   /* an install's download in flight (an update owns the device) */
static uint32_t s_dl_start;             /* hw_millis when it began */
static size_t s_dl_size;                /* the bytes the manifest says the image has */
static const char *s_dl_why;            /* why on_image stopped it, NULL if it didn't */

/* Where the manifest is; NULL: nowhere, no checks (a simulator without
   --set update_url, so it never reaches GitHub on its own). */
static const char *manifest_url(void)
{
#ifdef ESP_PLATFORM
    return UPDATE_MANIFEST_URL;
#else
    return sim_update_url();
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

static bool on_body(void *ctx, const uint8_t *data, size_t len)
{
    (void)ctx;
    if (len > sizeof s_body - s_len) {
        s_too_big = true;
        return false;
    }
    memcpy(s_body + s_len, data, len);
    s_len += len;
    return true;
}

/* The check is over: the next one is a day away, or after one that failed
   a part of the day (10 minutes, then an hour, then the whole day). */
static void check_over(bool ok)
{
    httpc_free(s_http);
    s_http = NULL;
    check_sched_done(&s_sched, hw_millis(), ok, check_every());
}

/* Silent on the screen; what was known before stays. */
static void check_failed(const char *why)
{
    ESP_LOGW(TAG, "check failed: %s", why);
    snprintf(s_err, sizeof s_err, "%s", why);
    proto_eventf("update check-failed %s", why);
    check_over(false);
}

/* The release's manifest is in a later format: a firmware newer than this
   wrote it. Nothing to install from here, the menu says to do it by hand. */
static void check_manual(int format)
{
    ESP_LOGW(TAG, "check done: the release's manifest is in format %d, this firmware reads 1: a manual update", format);
    check_over(true);
    s_err[0] = 0;
    s_have = false;
    if (!s_manual) proto_eventf("update manual format=%d", format);
    s_manual = true;
}

static void check_done(void)
{
    update_manifest_t m;
    char err[64];
    if (!update_manifest_parse(s_body, s_len, &m, err, sizeof err)) {
        if (m.format > 1) { check_manual(m.format); return; }
        char why[80];
        snprintf(why, sizeof why, "manifest: %s", err);
        check_failed(why);
        return;
    }
    bool newer = !strcmp(m.board, BOARD_NAME) && update_is_newer(m.version, fw_version());
    ESP_LOGI(TAG, "check done: %s for %s, this is %s: %s", m.version, m.board, fw_version(),
             newer ? "an update" : "nothing to offer");
    check_over(true);
    s_err[0] = 0;
    s_have = newer;
    s_manual = false;
    if (!newer) return;
    s_release = m;
    if (strcmp(s_told, m.version)) {
        snprintf(s_told, sizeof s_told, "%s", m.version);
        proto_eventf("update available ver=%s", m.version);
    }
}

static void check_start(void)
{
    const char *url = manifest_url();
    s_len = 0;
    s_too_big = false;
    if (strncmp(url, "https://", 8)) {
        check_failed("the manifest address isn't https");
        return;
    }
    char err[80];
    s_http = httpc_get(url, on_body, NULL, err, sizeof err);
    if (!s_http) check_failed(err);
}

/* Why an install failed goes on the screen as "update failed: <why>", one
   toast line of about 53 characters: the reasons below and
   httpc_short_error's fit it. The full story goes to the log and to EVT
   update install-failed. */
static const char WRONG_SIZE[] = "wrong size";
static const char TOO_LONG[] = "took too long";


/* The final answer says a length other than the manifest's. */
static bool wrong_length(void)
{
    long n = httpc_length(s_dl);
    return n >= 0 && (size_t)n != s_dl_size;
}

/* The image's bytes go straight into the other slot. */
static bool on_image(void *ctx, const uint8_t *data, size_t len)
{
    (void)ctx;
    if (wrong_length()) s_dl_why = WRONG_SIZE;
    else if (ota_write(data, len) < len) s_dl_why = "download too big";
    return !s_dl_why;
}

/* The download is over: ota_finish (which reboots, or reports its own
   failure), or ota_abort with why, short (the menu comes back with it);
   `full` is the whole story, NULL if why says it all. */
static void install_over(const char *why, const char *full)
{
    size_t size, got = ota_received(&size);
    if (why) {
        ESP_LOGW(TAG, "install failed at %zu/%zu: %s", got, size, full ? full : why);
        proto_eventf("update install-failed got=%zu/%zu %s", got, size, full ? full : why);
        ota_abort(why);
    }
    httpc_free(s_dl);                   /* (after: why and full may be its) */
    s_dl = NULL;
    if (why) return;
    char msg[200];
    if (ota_finish(msg, sizeof msg)) ESP_LOGI(TAG, "installed %s", msg);
    else ESP_LOGW(TAG, "install failed: %s", msg);
}

static void install_poll(void)
{
    /* Without Wi-Fi it would only time out, a while later. */
    if (!wifi_up()) { install_over("Wi-Fi dropped", NULL); return; }
    httpc_status_t st = httpc_poll(s_dl);
    if (st == HTTPC_BUSY) {
        if (hw_millis() - s_dl_start > UPDATE_INSTALL_MAX_MS) install_over(TOO_LONG, NULL);
        return;
    }
    size_t size, got = ota_received(&size);
    char buf[24];
    if (s_dl_why) install_over(s_dl_why, NULL);
    else if (st == HTTPC_FAILED) install_over(httpc_short_error(httpc_error(s_dl), buf, sizeof buf), httpc_error(s_dl));
    else if (wrong_length()) install_over(WRONG_SIZE, NULL);
    else if (got < size) install_over("download cut short", NULL);
    else install_over(NULL, NULL);
}

void update_init(void)
{
    const char *url = manifest_url();
    if (url) ESP_LOGI(TAG, "releases from %s", url);
    else ESP_LOGI(TAG, "no manifest address: no update checks");
}

void update_poll(void)
{
    if (s_dl) install_poll();
    if (s_http) {
        switch (httpc_poll(s_http)) {
        case HTTPC_BUSY: return;
        case HTTPC_DONE: check_done(); return;
        case HTTPC_FAILED: check_failed(s_too_big ? "manifest too big" : httpc_error(s_http)); return;
        }
    }
    if (!settings_update_check() || !manifest_url() || !wifi_up()) return;
    if (!check_sched_due(&s_sched, hw_millis(), UPDATE_FIRST_CHECK_MS) || ota_active()) return;
    check_start();
}

const update_manifest_t *update_available(void) { return s_have ? &s_release : NULL; }

void update_check_now(void)
{
    if (s_http || s_dl || !manifest_url() || !wifi_up() || ota_active()) return;
    check_start();
}

bool update_checking(void) { return s_http != NULL; }

const char *update_error(void) { return s_err; }

bool update_manual(void) { return s_manual; }

bool update_install(char *err, size_t cap)
{
    const update_manifest_t *u = update_available();
    if (!u) { snprintf(err, cap, "no update known"); return false; }
    if (ota_active()) { snprintf(err, cap, "an update is already running"); return false; }
    if (!wifi_up()) { snprintf(err, cap, "no Wi-Fi"); return false; }
    if (strncmp(u->url, "https://", 8)) { snprintf(err, cap, "the release address isn't https"); return false; }
    /* A check under way gives way to it: its turn comes again once this
       is over. */
    httpc_free(s_http);
    s_http = NULL;
    /* The download first, so nothing is shown if it can't even start; its
       bytes come only from update_poll, once ota_begin has the slot. */
    s_dl_size = u->size;
    s_dl_why = NULL;
    s_dl_start = hw_millis();
    s_dl = httpc_get(u->url, on_image, NULL, err, cap);
    if (!s_dl) return false;
    if (!ota_begin(u->size, u->sha256, err, cap)) {
        httpc_free(s_dl);
        s_dl = NULL;
        return false;
    }
    ESP_LOGI(TAG, "installing %s: %zu bytes from %s", u->version, u->size, u->url);
    return true;
}

void update_setting_changed(void)
{
    if (!settings_update_check()) {
        httpc_free(s_http);
        s_http = NULL;
        s_have = s_manual = false;
        s_told[0] = 0;
        check_sched_reset(&s_sched);
        return;
    }
    if (s_http) return;                  /* the check under way sets the next */
    check_sched_reset(&s_sched);         /* else when Wi-Fi comes up */
    if (wifi_up()) check_sched_soon(&s_sched, hw_millis(), UPDATE_FIRST_CHECK_MS);
}
