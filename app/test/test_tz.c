#include <string.h>

#include "check.h"
#include "tz.h"

void test_tz(int *total, int *failed);

static bool parse_time(const char *s, const tz_t *zone, int64_t *out)
{
    return tz_parse_time(s, strlen(s), zone, out);
}

/* Time zones by POSIX TZ string and the dates web APIs send (sys.date,
   sys.time). Reference times from Python's calendar.timegm. */
void test_tz(int *total, int *failed)
{
    /* the calendar, both ways, over the whole range */
    CHECK(tz_days_from_civil(1970, 1, 1) == 0);
    CHECK(tz_days_from_civil(1969, 12, 31) == -1);
    CHECK(tz_days_from_civil(2000, 3, 1) == 11017);
    CHECK(tz_days_from_civil(0, 1, 1) * 86400 == TZ_MIN_TIME);
    CHECK(tz_days_from_civil(9999, 12, 31) * 86400 + 86399 == TZ_MAX_TIME);
    bool round_trip = true;
    for (int64_t z = TZ_MIN_TIME / 86400; z <= TZ_MAX_TIME / 86400; z += 37) {
        int64_t y;
        int m, d;
        tz_civil_from_days(z, &y, &m, &d);
        round_trip = round_trip && tz_days_from_civil(y, m, d) == z;
    }
    CHECK(round_trip);

    /* fields carried over, as os.time does */
    int64_t l;
    CHECK(tz_fields(2026, 11, 15, 10, 30, 0, &l) && l == 1794738600);
    CHECK(tz_fields(2025, 23, 15, 10, 30, 0, &l) && l == 1794738600);         /* month 23 of 2025 */
    CHECK(tz_fields(2026, 12, -15, 10, 30, 0, &l) && l == 1794738600);        /* day -15 of December */
    CHECK(tz_fields(2026, 11, 15, 0, 630, 0, &l) && l == 1794738600);
    CHECK(tz_fields(2026, 1, 1, 0, 0, 0, &l) && l == 1767225600);
    CHECK(tz_fields(2026, 0, 1, 0, 0, 0, &l) && l == 1767225600 - 31 * 86400); /* month 0: December */
    CHECK(!tz_fields(2026, 1, 1, 0, 0, 2000000000, &l));

    /* TZ strings */
    tz_t fi, us, utc, syd, ie, fixed;
    CHECK(tz_parse("EET-2EEST,M3.5.0/3,M10.5.0/4", &fi));
    CHECK(tz_parse("EST5EDT", &us));                                          /* US rules by default */
    CHECK(tz_parse("UTC", &utc) && !utc.has_dst && utc.std_east == 0);
    CHECK(tz_parse("AEST-10AEDT,M10.1.0,M4.1.0/3", &syd));
    CHECK(tz_parse("IST-1GMT0,M10.5.0,M3.5.0/1", &ie));                        /* winter is the "dst" */
    CHECK(tz_parse("<+0530>-5:30", &fixed) && fixed.std_east == 19800 && !strcmp(fixed.std, "+0530"));
    CHECK(tz_parse("UTC0", &utc) && utc.std_east == 0);
    tz_t bad;
    CHECK(!tz_parse("", &bad));
    CHECK(!tz_parse("EET", &bad));
    CHECK(!tz_parse("EE-2", &bad));
    CHECK(!tz_parse("EET-25", &bad));
    CHECK(!tz_parse("EET-2EEST,M3.5.0", &bad));
    CHECK(!tz_parse("EET-2EEST,M13.5.0,M10.5.0", &bad));
    CHECK(!tz_parse("EET-2EEST,M3.5.0,M10.5.0x", &bad));
    CHECK(!tz_parse("Europe/Helsinki", &bad));
    CHECK(!tz_parse(":Europe/Helsinki", &bad));

    /* Helsinki: summer time 2026-03-29 01:00Z .. 2026-10-25 01:00Z */
    bool dst;
    CHECK(tz_offset(&fi, 1774746000 - 1, &dst) == 7200 && !dst);
    CHECK(tz_offset(&fi, 1774746000, &dst) == 10800 && dst);
    CHECK(tz_offset(&fi, 1792890000 - 1, &dst) == 10800 && dst);
    CHECK(tz_offset(&fi, 1792890000, &dst) == 7200 && !dst);
    /* US: 2026-03-08 07:00Z .. 2026-11-01 06:00Z */
    CHECK(tz_offset(&us, 1772953200 - 1, NULL) == -18000 && tz_offset(&us, 1772953200, NULL) == -14400);
    CHECK(tz_offset(&us, 1793512800 - 1, NULL) == -14400 && tz_offset(&us, 1793512800, NULL) == -18000);
    /* southern hemisphere, and a "dst" behind standard time */
    CHECK(tz_offset(&syd, 1767225600 + 14 * 86400, NULL) == 39600);            /* January: summer */
    CHECK(tz_offset(&syd, 1767225600 + 195 * 86400, NULL) == 36000);           /* July */
    CHECK(tz_offset(&ie, 1767225600 + 14 * 86400, &dst) == 0 && dst);
    CHECK(tz_offset(&ie, 1767225600 + 195 * 86400, &dst) == 3600 && !dst);

    tz_tm_t tm;
    tz_local(&fi, 1794738600, &tm);
    CHECK(tm.year == 2026 && tm.month == 11 && tm.day == 15 && tm.hour == 12 && tm.min == 30 && tm.sec == 0);
    CHECK(tm.wday == 0 && tm.yday == 318 && !tm.dst && tm.offset == 7200 && !strcmp(tm.zone, "EET"));
    tz_local(&fi, 1774746000, &tm);
    CHECK(tm.hour == 4 && tm.dst && !strcmp(tm.zone, "EEST"));
    tz_local(&utc, -1, &tm);
    CHECK(tm.year == 1969 && tm.month == 12 && tm.day == 31 && tm.hour == 23 && tm.sec == 59 && tm.wday == 3);
    tz_local(&utc, TZ_MIN_TIME, &tm);
    CHECK(tm.year == 0 && tm.month == 1 && tm.day == 1 && tm.wday == 6);       /* a Saturday */

    /* local to UTC, around the changes */
    CHECK(tz_fields(2026, 11, 15, 12, 30, 0, &l) && tz_from_local(&fi, l) == 1794738600);
    CHECK(tz_fields(2026, 3, 29, 3, 30, 0, &l) && tz_from_local(&fi, l) == 1774746000 + 1800); /* skipped: 04:30 */
    CHECK(tz_fields(2026, 3, 29, 2, 59, 59, &l) && tz_from_local(&fi, l) == 1774746000 - 1);
    CHECK(tz_fields(2026, 10, 25, 3, 30, 0, &l) && tz_from_local(&fi, l) == 1792890000 - 1800); /* twice: the first */
    CHECK(tz_fields(2026, 10, 25, 4, 0, 0, &l) && tz_from_local(&fi, l) == 1792890000 + 3600);
    bool inverse = true;                    /* local -> UTC -> local, every 7 h for two years */
    for (int64_t t = 1767225600; t < 1767225600 + 2 * 365 * 86400; t += 7 * 3600) {
        tz_local(&fi, t, &tm);
        inverse = inverse && tz_fields(tm.year, tm.month, tm.day, tm.hour, tm.min, tm.sec, &l) &&
                  (tz_from_local(&fi, l) == t || tz_offset(&fi, t - 3600, NULL) != tm.offset);
    }
    CHECK(inverse);

    /* date strings */
    int64_t t;
    CHECK(parse_time("2026-11-15T10:30:00Z", &fi, &t) && t == 1794738600);
    CHECK(parse_time("2026-11-15t10:30:00z", &fi, &t) && t == 1794738600);
    CHECK(parse_time("2026-11-15T12:30:00.250+02:00", &fi, &t) && t == 1794738600);
    CHECK(parse_time("2026-11-15T12:30:00,5+0200", &fi, &t) && t == 1794738600);
    CHECK(parse_time("2026-11-15T05:30:00-05", &fi, &t) && t == 1794738600);
    CHECK(parse_time("2026-11-15 10:30Z", &fi, &t) && t == 1794738600);
    CHECK(parse_time("2026-11-15T10:30:00", &utc, &t) && t == 1794738600);
    CHECK(parse_time("2026-11-15T12:30:00", &fi, &t) && t == 1794738600);      /* no offset: in the zone */
    CHECK(parse_time("2026-11-15", &fi, &t) && t == 1794700800 - 7200);        /* local midnight */
    CHECK(parse_time("Sun, 15 Nov 2026 10:30:00 GMT", &fi, &t) && t == 1794738600);
    CHECK(parse_time("15 Nov 2026 10:30:00 +0000", &fi, &t) && t == 1794738600);
    CHECK(parse_time("Sun, 15 Nov 2026 12:30:00 +0200", &fi, &t) && t == 1794738600);
    CHECK(parse_time("Sun, 5 Nov 2026 10:30:00 UTC", &fi, &t) && t == 1794738600 - 10 * 86400);
    CHECK(parse_time("0000-01-01T00:00:00Z", &fi, &t) && t == TZ_MIN_TIME);
    CHECK(parse_time("9999-12-31T23:59:59Z", &fi, &t) && t == TZ_MAX_TIME);
    CHECK(!parse_time("0000-01-01T00:00:00+01:00", &fi, &t));                  /* before year 0 in UTC */
    CHECK(!parse_time("", &fi, &t));
    CHECK(!parse_time("hello", &fi, &t));
    CHECK(!parse_time("2026-02-29", &fi, &t));
    CHECK(!parse_time("2026-11-15T24:00:00Z", &fi, &t));
    CHECK(!parse_time("2026-11-15T10:60Z", &fi, &t));
    CHECK(!parse_time("2026-11-15T10:30:00Q", &fi, &t));
    CHECK(!parse_time("2026-11-15T10:30:00Z ", &fi, &t));
    CHECK(!parse_time("2026-11-15T10:30:00.Z", &fi, &t));
    CHECK(!parse_time("2026-11-15T10:30:00+2:00", &fi, &t));
    CHECK(!parse_time("2026-1-15", &fi, &t));
    CHECK(!parse_time("Sun, 15 Foo 2026 10:30:00 GMT", &fi, &t));
    CHECK(parse_time("Sun, 15 Nov 2026 05:30:00 EST", &fi, &t) && t == 1794738600);
    CHECK(parse_time("Sun, 15 Nov 2026 03:30:00 pdt", &fi, &t) && t == 1794738600);
    CHECK(parse_time("Sun, 15 Nov 2026 10:30:00 UT", &fi, &t) && t == 1794738600);
    CHECK(parse_time("SUN, 15 NOV 2026 10:30:00 Z", &fi, &t) && t == 1794738600);
    CHECK(parse_time("Sunday,15  November 26 10:30 GMT", &fi, &t) && t == 1794738600);
    CHECK(parse_time("15 Nov 99 10:30 GMT", &fi, &t) && t == 942661800);         /* 50..99 is 19xx */
    CHECK(parse_time("15 Nov 2026 12:30", &fi, &t) && t == 1794738600);           /* no zone: in the zone */
    CHECK(!parse_time("Sun, 15 Nov 2026 10:30:00 XYZ", &fi, &t));
    CHECK(!parse_time("Sun, 15 No 2026 10:30:00 GMT", &fi, &t));
    CHECK(parse_time("Sun 15 Nov 2026 10:30:00 GMT", &fi, &t) && t == 1794738600);
    CHECK(!parse_time("Sun15 Nov 2026 10:30:00 GMT", &fi, &t));
    CHECK(!parse_time("15 Nov 2026 10:30:00 GMT ", &fi, &t));
    CHECK(parse_time("15 Nov 2026 12:30:00 +02", &fi, &t) && t == 1794738600);
    CHECK(tz_parse_time("2026-11-15T10:30:00Z", 10, &fi, &t) && t == 1794700800 - 7200);   /* len counts */

    /* tz_format: Sunday 2026-03-01 09:05:07 in Finland (EET, +0200), then
       a summer afternoon (EEST) and New York's winter (-0500). */
    char out[64];
    tz_local(&fi, 1772348707, &tm);
    CHECK(tz_format(out, sizeof out, "%Y-%m-%d %H:%M:%S", &tm) == 19 && !strcmp(out, "2026-03-01 09:05:07"));
    CHECK(tz_format(out, sizeof out, "%a %-d %b, %A %B %y", &tm) > 0 &&
          !strcmp(out, "Sun 1 Mar, Sunday March 26"));
    CHECK(tz_format(out, sizeof out, "%I:%M %p %-I %-H %j %Z %z 100%%", &tm) > 0 &&
          !strcmp(out, "09:05 AM 9 9 060 EET +0200 100%"));
    tz_local(&fi, 1784818800, &tm);                                             /* 2026-07-23 15:00 UTC */
    CHECK(tz_format(out, sizeof out, "%H %I %p %Z %z %-m", &tm) > 0 && !strcmp(out, "18 06 PM EEST +0300 7"));
    tz_local(&us, 1772348707, &tm);
    CHECK(tz_format(out, sizeof out, "%z %Z %H", &tm) > 0 && !strcmp(out, "-0500 EST 02"));
    tz_local(&utc, 0, &tm);
    CHECK(tz_format(out, sizeof out, "%I %p", &tm) == 5 && !strcmp(out, "12 AM"));
    CHECK(tz_format(out, sizeof out, "", &tm) == 0 && !strcmp(out, ""));
    CHECK(tz_format(out, sizeof out, "%Q", &tm) == TZ_FORMAT_BAD);
    CHECK(tz_format(out, sizeof out, "50%", &tm) == TZ_FORMAT_BAD);
    CHECK(tz_format(out, sizeof out, "%-a", &tm) == TZ_FORMAT_BAD);
    CHECK(tz_format(out, 5, "%Y-%m", &tm) == TZ_FORMAT_LONG);
    CHECK(tz_format(out, 8, "%Y-%m", &tm) == 7 && !strcmp(out, "1970-01"));

    *total += check_total;
    *failed += check_failed;
}
