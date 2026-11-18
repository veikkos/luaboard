#include "tz.h"

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------ calendar */

static int64_t floor_div(int64_t a, int64_t b) { return a / b - ((a % b != 0) && ((a < 0) != (b < 0))); }
static int64_t floor_mod(int64_t a, int64_t b) { return a - floor_div(a, b) * b; }

static bool leap(int64_t y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

static int month_days(int64_t y, int m)
{
    static const int days[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    return days[m - 1] + (m == 2 && leap(y));
}

int64_t tz_days_from_civil(int64_t y, int m, int d)
{
    if (m <= 2) y--;
    int64_t era = floor_div(y, 400);
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

void tz_civil_from_days(int64_t z, int64_t *y, int *m, int *d)
{
    z += 719468;
    int64_t era = floor_div(z, 146097);
    int64_t doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = yoe + era * 400 + (*m <= 2);
}

#define FIELD_MAX 1000000000LL         /* any field's size: no overflow below */

bool tz_fields(int64_t year, int64_t month, int64_t day, int64_t hour, int64_t min, int64_t sec,
               int64_t *local)
{
    if (year < -FIELD_MAX || year > FIELD_MAX || month < -FIELD_MAX || month > FIELD_MAX ||
        day < -FIELD_MAX || day > FIELD_MAX || hour < -FIELD_MAX || hour > FIELD_MAX ||
        min < -FIELD_MAX || min > FIELD_MAX || sec < -FIELD_MAX || sec > FIELD_MAX)
        return false;
    year += floor_div(month - 1, 12);
    int m = (int)floor_mod(month - 1, 12) + 1;
    if (year < -FIELD_MAX || year > FIELD_MAX) return false;
    int64_t days = tz_days_from_civil(year, m, 1) + day - 1;
    *local = days * 86400 + hour * 3600 + min * 60 + sec;
    return true;
}

/* ------------------------------------------------------- TZ strings */

static bool is_alpha(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }
static bool is_digit(char c) { return c >= '0' && c <= '9'; }

static bool parse_name(const char **p, char *out, size_t cap)
{
    const char *s = *p;
    size_t n = 0;
    if (*s == '<') {
        s++;
        while (*s && *s != '>') {
            if (!is_alpha(*s) && !is_digit(*s) && *s != '+' && *s != '-') return false;
            if (n + 1 < cap) out[n] = *s;
            s++;
            n++;
        }
        if (*s != '>') return false;
        s++;
    } else {
        while (is_alpha(*s)) {
            if (n + 1 < cap) out[n] = *s;
            s++;
            n++;
        }
    }
    if (n < 3 || n >= cap) return false;
    out[n] = 0;
    *p = s;
    return true;
}

static bool number(const char **p, int maxd, int maxv, int *out)
{
    int n = 0, v = 0;
    while (is_digit(**p) && n < maxd) { v = v * 10 + (**p - '0'); (*p)++; n++; }
    if (n == 0 || v > maxv) return false;
    *out = v;
    return true;
}

/* [+-]hh[:mm[:ss]] in seconds; `hours` = the largest hh allowed. */
static bool parse_hms(const char **p, int hours, int32_t *out)
{
    int sign = 1, h, m = 0, s = 0;
    if (**p == '+' || **p == '-') sign = *(*p)++ == '-' ? -1 : 1;
    if (!number(p, 3, hours, &h)) return false;
    if (**p == ':') { (*p)++; if (!number(p, 2, 59, &m)) return false; }
    if (**p == ':') { (*p)++; if (!number(p, 2, 59, &s)) return false; }
    *out = sign * (h * 3600 + m * 60 + s);
    return true;
}

static bool parse_rule(const char **p, tz_rule_t *r)
{
    memset(r, 0, sizeof *r);
    if (**p == 'M') {
        (*p)++;
        r->kind = 'M';
        if (!number(p, 2, 12, &r->m) || r->m < 1 || *(*p)++ != '.' || !number(p, 1, 5, &r->w) || r->w < 1 ||
            *(*p)++ != '.' || !number(p, 1, 6, &r->d))
            return false;
    } else if (**p == 'J') {
        (*p)++;
        r->kind = 'J';
        if (!number(p, 3, 365, &r->n) || r->n < 1) return false;
    } else {
        r->kind = 'n';
        if (!number(p, 3, 365, &r->n)) return false;
    }
    r->time = 2 * 3600;
    if (**p == '/') {
        (*p)++;
        if (!parse_hms(p, 167, &r->time)) return false;
    }
    return true;
}

bool tz_parse(const char *s, tz_t *out)
{
    if (!s) return false;
    tz_t tz;
    memset(&tz, 0, sizeof tz);
    if (strcmp(s, "UTC") == 0) {
        memcpy(tz.std, "UTC", 4);
        *out = tz;
        return true;
    }
    const char *p = s;
    int32_t west;
    if (!parse_name(&p, tz.std, sizeof tz.std) || !parse_hms(&p, 24, &west)) return false;
    tz.std_east = tz.dst_east = -west;
    if (*p) {
        if (!parse_name(&p, tz.dst, sizeof tz.dst)) return false;
        tz.has_dst = true;
        tz.dst_east = tz.std_east + 3600;
        if (*p && *p != ',') {
            if (!parse_hms(&p, 24, &west)) return false;
            tz.dst_east = -west;
        }
        const char *rules = *p ? p : ",M3.2.0,M11.1.0";      /* POSIX leaves it open; newlib: US rules */
        if (*rules++ != ',' || !parse_rule(&rules, &tz.start) || *rules++ != ',' || !parse_rule(&rules, &tz.end) ||
            *rules)
            return false;
    }
    *out = tz;
    return true;
}

/* The local day (days since 1970) on which the rule falls in year y. */
static int64_t rule_day(const tz_rule_t *r, int64_t y)
{
    int64_t jan1 = tz_days_from_civil(y, 1, 1);
    if (r->kind == 'J') return jan1 + r->n - 1 + (leap(y) && r->n >= 60);
    if (r->kind == 'n') return jan1 + r->n;
    int64_t first = tz_days_from_civil(y, r->m, 1);
    int wday1 = (int)floor_mod(first + 4, 7);             /* 1970-01-01 was a Thursday */
    int day = 1 + (r->d - wday1 + 7) % 7 + (r->w - 1) * 7;
    while (day > month_days(y, r->m)) day -= 7;           /* w = 5: the last such day */
    return first + day - 1;
}

int32_t tz_offset(const tz_t *tz, int64_t t, bool *dst)
{
    bool on = false;
    if (tz->has_dst) {
        int64_t y;
        int m, d;
        tz_civil_from_days(floor_div(t + tz->std_east, 86400), &y, &m, &d);
        int64_t start = rule_day(&tz->start, y) * 86400 + tz->start.time - tz->std_east;
        int64_t end = rule_day(&tz->end, y) * 86400 + tz->end.time - tz->dst_east;
        on = start < end ? (t >= start && t < end) : !(t >= end && t < start);
    }
    if (dst) *dst = on;
    return on ? tz->dst_east : tz->std_east;
}

void tz_local(const tz_t *tz, int64_t t, tz_tm_t *out)
{
    bool dst;
    int32_t off = tz_offset(tz, t, &dst);
    int64_t local = t + off;
    int64_t days = floor_div(local, 86400);
    int64_t secs = local - days * 86400;
    tz_civil_from_days(days, &out->year, &out->month, &out->day);
    out->hour = (int)(secs / 3600);
    out->min = (int)(secs % 3600 / 60);
    out->sec = (int)(secs % 60);
    out->wday = (int)floor_mod(days + 4, 7);
    out->yday = (int)(days - tz_days_from_civil(out->year, 1, 1));
    out->dst = dst;
    out->offset = off;
    out->zone = dst ? tz->dst : tz->std;
}

int64_t tz_from_local(const tz_t *tz, int64_t local)
{
    if (!tz->has_dst) return local - tz->std_east;
    int32_t hi = tz->std_east > tz->dst_east ? tz->std_east : tz->dst_east;
    int32_t lo = tz->std_east > tz->dst_east ? tz->dst_east : tz->std_east;
    if (tz_offset(tz, local - hi, NULL) == hi) return local - hi;   /* the earlier of two */
    if (tz_offset(tz, local - lo, NULL) == lo) return local - lo;
    return local - lo;                     /* skipped: as if the clocks hadn't moved yet */
}

/* ------------------------------------------------------- date strings */

typedef struct {
    const char *s, *end;
} scan_t;

static bool at_end(const scan_t *c) { return c->s >= c->end; }
static char peek(const scan_t *c) { return at_end(c) ? 0 : *c->s; }

static bool lit(scan_t *c, char ch)
{
    if (peek(c) != ch) return false;
    c->s++;
    return true;
}

/* Exactly n digits. */
static bool digits(scan_t *c, int n, int *out)
{
    int v = 0;
    for (int i = 0; i < n; i++) {
        if (!is_digit(peek(c))) return false;
        v = v * 10 + (*c->s++ - '0');
    }
    *out = v;
    return true;
}

/* Z, or +hh:mm / +hhmm / +hh. *has = false if there is none. */
static bool zone_suffix(scan_t *c, bool *has, int32_t *east)
{
    *has = false;
    if (lit(c, 'Z') || lit(c, 'z')) { *has = true; *east = 0; return true; }
    char sign = peek(c);
    if (sign != '+' && sign != '-') return true;
    c->s++;
    int h, m = 0;
    if (!digits(c, 2, &h) || h > 23) return false;
    if (lit(c, ':')) { if (!digits(c, 2, &m)) return false; }
    else if (is_digit(peek(c)) && !digits(c, 2, &m)) return false;
    if (m > 59) return false;
    *has = true;
    *east = (sign == '-' ? -1 : 1) * (h * 3600 + m * 60);
    return true;
}

static bool valid_date(int64_t y, int m, int d)
{
    return y >= 0 && y <= 9999 && m >= 1 && m <= 12 && d >= 1 && d <= month_days(y, m);
}

/* 2026-11-15[(T| )hh:mm[:ss[.fff]]][zone] */
static bool parse_iso(scan_t *c, int64_t *local, bool *has_zone, int32_t *east)
{
    int y, mo, d, h = 0, mi = 0, se = 0;
    if (!digits(c, 4, &y) || !lit(c, '-') || !digits(c, 2, &mo) || !lit(c, '-') || !digits(c, 2, &d)) return false;
    if (!valid_date(y, mo, d)) return false;
    *has_zone = false;
    if (lit(c, 'T') || lit(c, 't') || lit(c, ' ')) {
        if (!digits(c, 2, &h) || !lit(c, ':') || !digits(c, 2, &mi)) return false;
        if (lit(c, ':')) {
            if (!digits(c, 2, &se)) return false;
            if (lit(c, '.') || lit(c, ',')) {
                if (!is_digit(peek(c))) return false;
                while (is_digit(peek(c))) c->s++;
            }
        }
        if (h > 23 || mi > 59 || se > 60) return false;              /* 60: a leap second */
        if (!zone_suffix(c, has_zone, east)) return false;
    }
    return tz_fields(y, mo, d, h, mi, se, local);
}

static char lower(char ch) { return ch >= 'A' && ch <= 'Z' ? (char)(ch - 'A' + 'a') : ch; }

/* One or more spaces or tabs. */
static bool blanks(scan_t *c)
{
    if (peek(c) != ' ' && peek(c) != '\t') return false;
    while (peek(c) == ' ' || peek(c) == '\t') c->s++;
    return true;
}

/* A run of letters, any case, as a lower-case word of at most cap - 1. */
static bool word(scan_t *c, char *out, size_t cap)
{
    size_t n = 0;
    while (is_alpha(peek(c))) {
        if (n + 1 >= cap) return false;
        out[n++] = lower(*c->s++);
    }
    out[n] = 0;
    return n > 0;
}

/* RFC 822's named zones (and UT, UTC, Z), hours east of UTC. */
static bool named_zone(const char *w, int32_t *east)
{
    static const struct { char name[4]; int8_t hours; } zones[] = {
        { "gmt", 0 },  { "ut", 0 },   { "utc", 0 },  { "z", 0 },    { "est", -5 }, { "edt", -4 },
        { "cst", -6 }, { "cdt", -5 }, { "mst", -7 }, { "mdt", -6 }, { "pst", -8 }, { "pdt", -7 },
    };
    for (size_t i = 0; i < sizeof zones / sizeof zones[0]; i++)
        if (!strcmp(w, zones[i].name)) {
            *east = zones[i].hours * 3600;
            return true;
        }
    return false;
}

/* [Sun[,] ]15 Nov 2026 10:30[:00] [zone], as HTTP and RSS (RFC 822) send it:
 * any case, the month's name in full or not, a two-digit year (00..49 is
 * 20xx, 50..99 19xx), the zone a number (+hhmm) or a name (GMT, EST...). */
static bool parse_http(scan_t *c, int64_t *local, bool *has_zone, int32_t *east)
{
    static const char months[12][4] = { "jan", "feb", "mar", "apr", "may", "jun",
                                        "jul", "aug", "sep", "oct", "nov", "dec" };
    char w[12];
    if (is_alpha(peek(c))) {                                            /* the weekday: not checked */
        if (!word(c, w, sizeof w)) return false;
        bool comma = lit(c, ',');
        if (!blanks(c) && !comma) return false;
    }
    int d, mo = 0, y, h, mi, se = 0;
    if (!digits(c, 1, &d)) return false;                               /* 1 or 2 digits */
    if (is_digit(peek(c))) d = d * 10 + (*c->s++ - '0');
    if (!blanks(c) || !word(c, w, sizeof w) || strlen(w) < 3) return false;
    for (int i = 0; i < 12; i++)
        if (!memcmp(w, months[i], 3)) mo = i + 1;
    if (!mo || !blanks(c) || !digits(c, 2, &y)) return false;
    if (is_digit(peek(c))) {
        int lo;
        if (!digits(c, 2, &lo)) return false;
        y = y * 100 + lo;
    } else {
        y += y < 50 ? 2000 : 1900;
    }
    if (!blanks(c) || !digits(c, 2, &h) || !lit(c, ':') || !digits(c, 2, &mi)) return false;
    if (lit(c, ':') && !digits(c, 2, &se)) return false;
    if (!valid_date(y, mo, d) || h > 23 || mi > 59 || se > 60) return false;
    *has_zone = false;
    if (blanks(c)) {
        if (is_alpha(peek(c))) {
            if (!word(c, w, sizeof w) || !named_zone(w, east)) return false;
            *has_zone = true;
        } else if (!zone_suffix(c, has_zone, east) || !*has_zone) {
            return false;
        }
    }
    return tz_fields(y, mo, d, h, mi, se, local);
}

bool tz_parse_time(const char *s, size_t len, const tz_t *zone, int64_t *out)
{
    scan_t c = { s, s + len };
    int64_t local;
    bool has_zone;
    int32_t east = 0;
    bool ok = is_digit(peek(&c)) && c.end - c.s >= 5 && c.s[4] == '-' ? parse_iso(&c, &local, &has_zone, &east)
                                                                     : parse_http(&c, &local, &has_zone, &east);
    if (!ok || !at_end(&c)) return false;
    int64_t t = has_zone ? local - east : tz_from_local(zone, local);
    if (t < TZ_MIN_TIME || t > TZ_MAX_TIME) return false;
    *out = t;
    return true;
}

/* ------------------------------------------------------------ format */

static const char *const DAY_NAMES[7] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday",
                                          "Saturday" };
static const char *const MONTH_NAMES[12] = { "January", "February", "March",     "April",   "May",      "June",
                                             "July",    "August",   "September", "October", "November", "December" };

int tz_format(char *out, size_t cap, const char *fmt, const tz_tm_t *tm)
{
    size_t n = 0;
    if (!cap) return TZ_FORMAT_LONG;
    for (const char *f = fmt; *f; f++) {
        char piece[24];
        const char *add = piece;
        if (*f != '%') {
            piece[0] = *f;
            piece[1] = 0;
        } else {
            bool bare = f[1] == '-';
            if (bare) f++;
            int num = -1, width = 2;
            switch (*++f) {
            case 'Y': num = (int)tm->year; width = 1; break;
            case 'y': num = (int)(tm->year % 100); break;
            case 'm': num = tm->month; break;
            case 'd': num = tm->day; break;
            case 'H': num = tm->hour; break;
            case 'I': num = (tm->hour + 11) % 12 + 1; break;
            case 'M': num = tm->min; break;
            case 'S': num = tm->sec; break;
            case 'j': num = tm->yday + 1; width = 3; break;
            case 'p': add = tm->hour < 12 ? "AM" : "PM"; break;
            case 'a': snprintf(piece, sizeof piece, "%.3s", DAY_NAMES[tm->wday]); break;
            case 'A': add = DAY_NAMES[tm->wday]; break;
            case 'b': snprintf(piece, sizeof piece, "%.3s", MONTH_NAMES[tm->month - 1]); break;
            case 'B': add = MONTH_NAMES[tm->month - 1]; break;
            case 'Z': add = tm->zone ? tm->zone : ""; break;
            case 'z': {
                int32_t e = tm->offset < 0 ? -tm->offset : tm->offset;
                snprintf(piece, sizeof piece, "%c%02d%02d", tm->offset < 0 ? '-' : '+', (int)(e / 3600),
                         (int)(e / 60 % 60));
                break;
            }
            case '%': add = "%"; break;
            default: return TZ_FORMAT_BAD;          /* (a % at the end too) */
            }
            if (bare && num < 0) return TZ_FORMAT_BAD;
            if (num >= 0) snprintf(piece, sizeof piece, "%0*d", bare ? 1 : width, num);
        }
        size_t k = strlen(add);
        if (n + k >= cap) return TZ_FORMAT_LONG;
        memcpy(out + n, add, k);
        n += k;
    }
    out[n] = 0;
    return (int)n;
}
