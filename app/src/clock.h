/*
 * Wall clock: set by NTP once Wi-Fi has an address, or at boot from the RTC
 * chip if it kept its time. Shown in the device's time zone (POSIX TZ string).
 * Until then the time is unknown and scripts get nil from sys.time().
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include "esp_err.h"
#include "tz.h"

/** Apply the stored time zone. */
void clock_init(void);
/** Wi-Fi got an address: start NTP (once; it keeps itself in sync). */
void clock_network_up(void);
/** From the main loop: announces the first sync (EVT time ...). */
void clock_poll(void);
/** Ask the time server now (Settings > Time, TIME SYNC). ESP_ERR_INVALID_STATE
 *  if NTP hasn't started (no Wi-Fi since boot). */
esp_err_t clock_sync_now(void);
/** NTP syncs since boot, and the UTC time of the last one (0: none yet). */
unsigned clock_syncs(void);
int64_t clock_last_sync(void);

bool clock_valid(void);
/** Where the time came from: "ntp", "rtc" (the RTC chip, at boot; NTP not yet) or "-". */
const char *clock_source(void);
/** UTC seconds since 1970, or 0 if the time isn't known yet. */
int64_t clock_now(void);

/** Is `tz` a POSIX TZ string we accept (e.g. "EET-2EEST,M3.5.0/3,M10.5.0/4")? */
bool clock_tz_valid(const char *tz);
/** Check, store and apply a time zone. */
esp_err_t clock_set_tz(const char *tz);
const char *clock_tz(void);
/** The device's time zone, parsed (for tz.c). */
const tz_t *clock_zone(void);

/** Local time of `t` broken down (tm_isdst 0 or 1); *offset = seconds east of UTC. */
void clock_local(int64_t t, struct tm *out, int32_t *offset);
/** "2026-11-06T15:30:00+02:00" for `t`, or "-" if t == 0. */
void clock_iso_local(int64_t t, char *out, size_t cap);
