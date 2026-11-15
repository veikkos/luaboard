/*
 * Local time by a POSIX TZ string, as newlib does on the device (MSVC's
 * tzset() only knows "EST5EDT"-style strings with US rules, and its
 * strftime("%Z") names Windows' own zone). The firmware sets TZ to a POSIX
 * string such as "EET-2EEST,M3.5.0/3,M10.5.0/4" and calls tzset(); this
 * parses it, and localtime_r() and strftime("%Z") follow it. Without a TZ
 * (or with one that doesn't parse) local time is UTC, as on the device.
 *
 *   std offset [dst [offset] [,start[/time],end[/time]]]
 *   names: 3+ letters, or anything in <...>; offsets [+-]hh[:mm[:ss]], west
 *   of UTC positive; dates Mm.w.d, Jn (1..365, no Feb 29) or n (0..365);
 *   times default to 02:00 and may be negative or past 24 h.
 */
#include "posix/posix_compat.h"

#include <ctype.h>

typedef struct {
    char kind;          /* 'M', 'J' or 'n' */
    int m, w, d, n;
    long time;          /* seconds after local midnight */
} rule_t;

static struct {
    char std[16], dst[16];
    long std_east, dst_east;   /* seconds east of UTC */
    bool has_dst;
    rule_t start, end;
} s_tz = { "UTC", "", 0, 0, false };

static SRWLOCK s_lock = SRWLOCK_INIT;

static bool parse_name(const char **p, char *out, size_t cap)
{
    const char *s = *p;
    size_t n = 0;
    if (*s == '<') {
        s++;
        while (*s && *s != '>') { if (n + 1 < cap) out[n++] = *s; s++; }
        if (*s != '>') return false;
        s++;
    } else {
        while (isalpha((unsigned char)*s)) { if (n + 1 < cap) out[n++] = *s; s++; }
    }
    out[n] = 0;
    if (n < 3) return false;
    *p = s;
    return true;
}

/* [+-]hh[:mm[:ss]] in seconds. */
static bool parse_hms(const char **p, long *out)
{
    const char *s = *p;
    int sign = 1;
    if (*s == '+' || *s == '-') sign = *s++ == '-' ? -1 : 1;
    if (!isdigit((unsigned char)*s)) return false;
    long h = strtol(s, (char **)&s, 10), m = 0, sec = 0;
    if (*s == ':') { s++; m = strtol(s, (char **)&s, 10); }
    if (*s == ':') { s++; sec = strtol(s, (char **)&s, 10); }
    if (h > 167 || m > 59 || sec > 59) return false;
    *out = sign * (h * 3600 + m * 60 + sec);
    *p = s;
    return true;
}

static bool parse_rule(const char **p, rule_t *r)
{
    const char *s = *p;
    memset(r, 0, sizeof *r);
    if (*s == 'M') {
        r->kind = 'M';
        s++;
        r->m = (int)strtol(s, (char **)&s, 10);
        if (*s++ != '.') return false;
        r->w = (int)strtol(s, (char **)&s, 10);
        if (*s++ != '.') return false;
        r->d = (int)strtol(s, (char **)&s, 10);
        if (r->m < 1 || r->m > 12 || r->w < 1 || r->w > 5 || r->d < 0 || r->d > 6) return false;
    } else if (*s == 'J') {
        r->kind = 'J';
        s++;
        r->n = (int)strtol(s, (char **)&s, 10);
        if (r->n < 1 || r->n > 365) return false;
    } else if (isdigit((unsigned char)*s)) {
        r->kind = 'n';
        r->n = (int)strtol(s, (char **)&s, 10);
        if (r->n > 365) return false;
    } else {
        return false;
    }
    r->time = 2 * 3600;
    if (*s == '/' && (s++, !parse_hms(&s, &r->time))) return false;
    *p = s;
    return true;
}

static bool parse_tz(const char *tz)
{
    const char *p = tz;
    char std[16], dst[16] = "";
    long std_west, dst_west;
    if (*p == ':') return false;                     /* an implementation-defined file: not here */
    if (!parse_name(&p, std, sizeof std) || !parse_hms(&p, &std_west)) return false;
    bool has_dst = *p && *p != ',';
    rule_t start, end;
    if (has_dst) {
        if (!parse_name(&p, dst, sizeof dst)) return false;
        dst_west = std_west - 3600;
        if (*p && *p != ',' && !parse_hms(&p, &dst_west)) return false;
        const char *rules = *p == ',' ? p + 1 : "M3.2.0,M11.1.0";   /* POSIX leaves it open; US rules */
        if (!parse_rule(&rules, &start) || *rules++ != ',' || !parse_rule(&rules, &end) || *rules) return false;
        p += strlen(p);
    }
    if (*p) return false;
    snprintf(s_tz.std, sizeof s_tz.std, "%s", std);
    snprintf(s_tz.dst, sizeof s_tz.dst, "%s", dst);
    s_tz.std_east = -std_west;
    s_tz.dst_east = has_dst ? -dst_west : -std_west;
    s_tz.has_dst = has_dst;
    if (has_dst) { s_tz.start = start; s_tz.end = end; }
    return true;
}

void posix_tzset(void)
{
    const char *tz = getenv("TZ");
    AcquireSRWLockExclusive(&s_lock);
    if (!tz || !*tz || !parse_tz(tz)) {
        snprintf(s_tz.std, sizeof s_tz.std, "UTC");
        s_tz.dst[0] = 0;
        s_tz.std_east = s_tz.dst_east = 0;
        s_tz.has_dst = false;
    }
    ReleaseSRWLockExclusive(&s_lock);
}

static bool leap(long long y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

/* Days from 1970-01-01 to y-m-d. */
static long long days_from_civil(long long y, int m, int d)
{
    struct tm tm = { .tm_year = (int)(y - 1900), .tm_mon = m - 1, .tm_mday = d };
    return (long long)timegm(&tm) / 86400;
}

/* The local midnight that starts the rule's day in year y, as days since 1970. */
static long long rule_day(const rule_t *r, long long y)
{
    if (r->kind == 'J') {
        int n = r->n;                                 /* 1..365, Feb 29 never counted */
        if (leap(y) && n >= 60) n++;
        return days_from_civil(y, 1, 1) + n - 1;
    }
    if (r->kind == 'n') return days_from_civil(y, 1, 1) + r->n;
    static const int mdays[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    long long first = days_from_civil(y, r->m, 1);
    int wday1 = (int)(((first % 7) + 7 + 4) % 7);   /* 1970-01-01 was a Thursday */
    int day = 1 + (r->d - wday1 + 7) % 7 + (r->w - 1) * 7;
    int len = mdays[r->m - 1] + (r->m == 2 && leap(y));
    while (day > len) day -= 7;                       /* w=5: the last such day */
    return first + day - 1;
}

static bool is_dst(long long t)
{
    if (!s_tz.has_dst) return false;
    struct tm tm;
    time_t local = (time_t)(t + s_tz.std_east);
    if (!gmtime_r(&local, &tm)) return false;
    long long y = tm.tm_year + 1900LL;
    long long start = rule_day(&s_tz.start, y) * 86400 + s_tz.start.time - s_tz.std_east;
    long long end = rule_day(&s_tz.end, y) * 86400 + s_tz.end.time - s_tz.dst_east;
    return start < end ? (t >= start && t < end) : !(t >= end && t < start);
}

struct tm *localtime_r(const time_t *t, struct tm *out)
{
    AcquireSRWLockShared(&s_lock);
    bool dst = is_dst((long long)*t);
    time_t local = *t + (dst ? s_tz.dst_east : s_tz.std_east);
    ReleaseSRWLockShared(&s_lock);
    if (!gmtime_r(&local, out)) return NULL;
    out->tm_isdst = dst;
    return out;
}

size_t posix_strftime(char *out, size_t cap, const char *fmt, const struct tm *tm)
{
    /* %Z is the zone's abbreviation from TZ; the rest is the CRT's. */
    char f[256];
    size_t n = 0;
    for (const char *p = fmt; *p && n + 20 < sizeof f; p++) {
        if (p[0] == '%' && p[1] == 'Z') {
            AcquireSRWLockShared(&s_lock);
            const char *z = tm->tm_isdst > 0 && s_tz.has_dst ? s_tz.dst : s_tz.std;
            for (; *z && n + 4 < sizeof f; z++) {
                if (*z == '%') f[n++] = '%';
                f[n++] = *z;
            }
            ReleaseSRWLockShared(&s_lock);
            p++;
        } else if (p[0] == '%' && p[1]) {
            f[n++] = *p++;
            f[n++] = *p;
        } else {
            f[n++] = *p;
        }
    }
    f[n] = 0;
#undef strftime
    return strftime(out, cap, f, tm);
}
