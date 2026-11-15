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

static const char *TAG = "clock";

static atomic_bool s_synced;          /* set from the SNTP task */
static atomic_uint s_syncs;           /* NTP syncs so far (the SNTP task counts) */
static unsigned s_saved_syncs;        /* ...of which the RTC chip has been given */
static bool s_from_rtc;               /* time taken from the RTC chip at boot */
static int64_t s_last_sync;           /* UTC of the last NTP sync the main loop saw */
static bool s_started, s_announced;

static void apply_tz(const char *tz)
{
    setenv("TZ", tz, 1);
    tzset();
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

bool clock_valid(void) { return s_from_rtc || atomic_load(&s_synced); }

const char *clock_source(void)
{
    return atomic_load(&s_synced) ? "ntp" : s_from_rtc ? "rtc" : "-";
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

/* std/dst name: 3+ letters, or <...> with letters, digits, + and - */
static bool tz_name(const char **p)
{
    const char *s = *p;
    int n = 0;
    if (*s == '<') {
        s++;
        while (*s && *s != '>') {
            if (!((*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') ||
                  *s == '+' || *s == '-')) return false;
            s++; n++;
        }
        if (*s != '>' || n < 3) return false;
        *p = s + 1;
        return true;
    }
    while ((*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z')) { s++; n++; }
    if (n < 3) return false;
    *p = s;
    return true;
}

static bool digits(const char **p, int maxd, int maxv)
{
    int n = 0, v = 0;
    while (**p >= '0' && **p <= '9' && n < maxd) { v = v * 10 + (**p - '0'); (*p)++; n++; }
    return n > 0 && v <= maxv;
}

/* [+-]hh[:mm[:ss]]; `hours` = largest hour value allowed */
static bool tz_time(const char **p, bool sign, int hours)
{
    if (sign && (**p == '+' || **p == '-')) (*p)++;
    if (!digits(p, 3, hours)) return false;
    for (int i = 0; i < 2 && **p == ':'; i++) {
        (*p)++;
        if (!digits(p, 2, 59)) return false;
    }
    return true;
}

/* Mm.w.d | Jn | n, then optionally /time */
static bool tz_rule(const char **p)
{
    if (**p == 'M') {
        (*p)++;
        if (!digits(p, 2, 12) || *(*p)++ != '.' || !digits(p, 1, 5) || *(*p)++ != '.' || !digits(p, 1, 6))
            return false;
    } else if (**p == 'J') {
        (*p)++;
        if (!digits(p, 3, 365)) return false;
    } else if (!digits(p, 3, 365)) {
        return false;
    }
    if (**p == '/') { (*p)++; return tz_time(p, true, 167); }
    return true;
}

bool clock_tz_valid(const char *tz)
{
    if (!tz || !*tz || strlen(tz) >= TZ_MAX) return false;
    const char *p = tz;
    if (!tz_name(&p) || !tz_time(&p, true, 24)) return false;
    if (!*p) return true;                               /* no daylight saving */
    if (!tz_name(&p)) return false;
    if (*p && *p != ',' && !tz_time(&p, true, 24)) return false;
    if (!*p) return true;
    if (*p++ != ',' || !tz_rule(&p) || *p++ != ',' || !tz_rule(&p)) return false;
    return *p == 0;
}

esp_err_t clock_set_tz(const char *tz)
{
    if (!clock_tz_valid(tz)) return ESP_ERR_INVALID_ARG;
    esp_err_t err = settings_set_timezone(tz);
    apply_tz(tz);
    return err;
}

const char *clock_tz(void) { return settings_timezone(); }

/* ------------------------------------------------------- local time */

void clock_local(int64_t t, struct tm *out, int32_t *offset)
{
    time_t tt = (time_t)t;
    localtime_r(&tt, out);
    if (offset) {
        struct tm as_utc = *out;
        *offset = (int32_t)(timegm(&as_utc) - t);
    }
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
