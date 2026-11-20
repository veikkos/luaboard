#include "clock.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "app_config.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "hw.h"
#include "protocol.h"
#include "settings.h"
#include "tz.h"

static const char *TAG = "clock";

static atomic_bool s_synced;          /* set from the SNTP task */
static atomic_uint s_syncs;           /* NTP syncs so far (the SNTP task counts) */
static unsigned s_saved_syncs;        /* ...of which the RTC chip has been given */
static bool s_from_rtc;               /* time taken from the RTC chip at boot */
static bool s_from_usb;               /* time taken from the USB tether's computer */
static int64_t s_last_sync;           /* UTC of the last NTP sync the main loop saw */
static bool s_started, s_announced;

static tz_t s_zone;                   /* the device's time zone, parsed */

/* The C library's local time isn't used: tz.c is the same everywhere. */
static void apply_tz(const char *tz)
{
    if (!tz_parse(tz, &s_zone)) tz_parse(TZ_DEFAULT, &s_zone);
}

#define SANE_TIME 1735689600          /* 2025-01-01: anything earlier is a chip that was never set */

void clock_init(void)
{
    const char *tz = settings_timezone();
    apply_tz(clock_tz_valid(tz) ? tz : TZ_DEFAULT);
    /* The RTC chip keeps time while the board has power (reset button, a
       crash, a firmware update) and, with a battery, while it's off. */
    int64_t t;
    if (hw_rtc_read(&t) && t >= SANE_TIME) {
        struct timeval tv = { .tv_sec = (time_t)t };
        settimeofday(&tv, NULL);
        s_from_rtc = true;
        ESP_LOGI(TAG, "time from the RTC chip: %lld", (long long)t);
    }
}

static void on_sync(struct timeval *tv)
{
    (void)tv;
    atomic_store(&s_synced, true);
    atomic_fetch_add(&s_syncs, 1);
}

void clock_network_up(void)
{
    if (s_started) return;
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(NTP_SERVER);
    cfg.sync_cb = on_sync;
    esp_err_t err = esp_netif_sntp_init(&cfg);
    if (err != ESP_OK) { ESP_LOGW(TAG, "SNTP start failed: %s", esp_err_to_name(err)); return; }
    s_started = true;
}

esp_err_t clock_sync_now(void)
{
    if (!s_started) return ESP_ERR_INVALID_STATE;
    return esp_netif_sntp_start();                 /* (re)start: asks the server now */
}

unsigned clock_syncs(void) { return s_saved_syncs; }
int64_t clock_last_sync(void) { return s_last_sync; }

void clock_set_usb(int64_t utc)
{
    if (atomic_load(&s_synced) || utc < SANE_TIME) return;
    struct timeval tv = { .tv_sec = (time_t)utc };
    settimeofday(&tv, NULL);
    s_from_usb = true;
    esp_err_t err = hw_rtc_write(utc);
    if (err != ESP_OK) ESP_LOGW(TAG, "RTC write: %s", esp_err_to_name(err));
    char iso[40];
    clock_iso_local(utc, iso, sizeof iso);
    proto_eventf("time usb %s", iso);
}

bool clock_valid(void) { return s_from_rtc || s_from_usb || atomic_load(&s_synced); }

const char *clock_source(void)
{
    return atomic_load(&s_synced) ? "ntp" : s_from_usb ? "usb" : s_from_rtc ? "rtc" : "-";
}

int64_t clock_now(void)
{
    if (!clock_valid()) return 0;
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec;
}

void clock_poll(void)
{
    /* After each NTP sync, give the RTC chip the time (from here, not the
       SNTP task, so the I2C bus has one user). */
    unsigned n = atomic_load(&s_syncs);
    if (n != s_saved_syncs) {
        s_saved_syncs = n;
        s_last_sync = clock_now();
        esp_err_t err = hw_rtc_write(s_last_sync);
        if (err != ESP_OK) ESP_LOGW(TAG, "RTC write: %s", esp_err_to_name(err));
    }
    /* Announce once each: the time from the RTC chip at boot, the first NTP sync. */
    static bool rtc_told;
    char iso[40];
    if (!s_announced && atomic_load(&s_synced)) {
        s_announced = true;
        clock_iso_local(clock_now(), iso, sizeof iso);
        proto_eventf("time synced %s", iso);
    } else if (!rtc_told && s_from_rtc && !atomic_load(&s_synced)) {
        rtc_told = true;
        clock_iso_local(clock_now(), iso, sizeof iso);
        proto_eventf("time rtc %s", iso);
    }
}

/* ------------------------------------------------------- time zones */

bool clock_tz_valid(const char *tz)
{
    tz_t parsed;
    return tz && strlen(tz) < TZ_MAX && tz_parse(tz, &parsed);
}

esp_err_t clock_set_tz(const char *tz)
{
    if (!clock_tz_valid(tz)) return ESP_ERR_INVALID_ARG;
    esp_err_t err = settings_set_timezone(tz);
    apply_tz(tz);
    return err;
}

const char *clock_tz(void) { return settings_timezone(); }

const tz_t *clock_zone(void) { return &s_zone; }

/* ------------------------------------------------------- local time */

void clock_local(int64_t t, struct tm *out, int32_t *offset)
{
    tz_tm_t tm;
    tz_local(&s_zone, t, &tm);
    memset(out, 0, sizeof *out);
    out->tm_year = (int)(tm.year - 1900);
    out->tm_mon = tm.month - 1;
    out->tm_mday = tm.day;
    out->tm_hour = tm.hour;
    out->tm_min = tm.min;
    out->tm_sec = tm.sec;
    out->tm_wday = tm.wday;
    out->tm_yday = tm.yday;
    out->tm_isdst = tm.dst;
    if (offset) *offset = tm.offset;
}

void clock_iso_local(int64_t t, char *out, size_t cap)
{
    if (t == 0) { snprintf(out, cap, "-"); return; }
    struct tm tm;
    int32_t off;
    clock_local(t, &tm, &off);
    int a = off < 0 ? -off : off;
    snprintf(out, cap, "%04d-%02d-%02dT%02d:%02d:%02d%c%02d:%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec, off < 0 ? '-' : '+', a / 3600, a % 3600 / 60);
}
