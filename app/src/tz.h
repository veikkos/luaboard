/*
 * Time zones and dates without the C library's local time: a POSIX TZ
 * string ("EET-2EEST,M3.5.0/3,M10.5.0/4") parsed once and evaluated for any
 * moment, the inverse (a local date and time to UTC), and a parser of the
 * date strings web APIs send (RFC 3339 / ISO 8601, HTTP dates). The same
 * code on the device and every host, for years 0..9999.
 *
 *   std offset [dst [offset] [,start[/time],end[/time]]]
 *   names: 3+ letters, or <...> with letters, digits, + and -; offsets
 *   [+-]hh[:mm[:ss]], west of UTC positive (hh 0..24); dates Mm.w.d,
 *   Jn (1..365, Feb 29 never counted) or n (0..365); times default to
 *   02:00 and may be negative or up to 167 h. A dst without rules takes
 *   the US ones (M3.2.0,M11.1.0), as newlib does. "UTC" alone is UTC0.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TZ_MIN_TIME (-62167219200LL)   /* 0000-01-01T00:00:00Z */
#define TZ_MAX_TIME 253402300799LL     /* 9999-12-31T23:59:59Z */

typedef struct {
    char kind;                         /* 'M', 'J' or 'n' */
    int m, w, d, n;
    int32_t time;                      /* seconds after local midnight */
} tz_rule_t;

typedef struct {
    char std[16], dst[16];
    int32_t std_east, dst_east;        /* seconds east of UTC */
    bool has_dst;
    tz_rule_t start, end;
} tz_t;

typedef struct {
    int64_t year;
    int month, day, hour, min, sec;    /* 1..12, 1..31, 0..23, 0..59, 0..59 */
    int wday, yday;                    /* 0 = Sunday; 0 = January 1st */
    bool dst;
    int32_t offset;                    /* seconds east of UTC */
    const char *zone;                  /* the abbreviation, inside the tz_t */
} tz_tm_t;

/** Parse a POSIX TZ string into *out. false if it isn't one. */
bool tz_parse(const char *s, tz_t *out);
/** Seconds east of UTC at UTC time t; *dst (if given) says whether it's daylight saving. */
int32_t tz_offset(const tz_t *tz, int64_t t, bool *dst);
/** t (UTC seconds) broken down in the zone. */
void tz_local(const tz_t *tz, int64_t t, tz_tm_t *out);
/** A local time, as seconds since 1970 read as if it were UTC, to UTC
 *  seconds. In the hour skipped when the clocks go forward, the offset of
 *  before the change counts (02:30 becomes 03:30); in the hour repeated
 *  when they go back, the first one. */
int64_t tz_from_local(const tz_t *tz, int64_t local);

/** Days from 1970-01-01 to y-m-d (proleptic Gregorian; m 1..12). */
int64_t tz_days_from_civil(int64_t y, int m, int d);
/** The date of day z since 1970-01-01. */
void tz_civil_from_days(int64_t z, int64_t *y, int *m, int *d);
/** Local seconds (as if UTC) of year/month/day hour:min:sec, the fields
 *  allowed out of range (month 13, day 0, min -30) and carried over, as
 *  os.time does. false if a field is so far out that it can't be counted. */
bool tz_fields(int64_t year, int64_t month, int64_t day, int64_t hour, int64_t min, int64_t sec,
               int64_t *local);

/** A date and time as web APIs give it, to UTC seconds:
 *    2026-11-15T10:30:00Z, 2026-11-15T12:30:00.250+02:00, 2026-11-15 10:30,
 *    2026-11-15 (midnight), Sun, 15 Nov 2026 10:30:00 GMT.
 *  Without an offset it's a time in `zone`. Fractions of a second are
 *  dropped. false if `s` isn't one (or is outside years 0..9999). */
bool tz_parse_time(const char *s, size_t len, const tz_t *zone, int64_t *out);
