/* POSIX functions MSVC lacks (posix/posix_compat.h, unistd.h, dirent.h, sys/time.h). */
#include <dirent.h>
#include <sys/time.h>
#include <unistd.h>

#include <bcrypt.h>

/* ---- strings ----------------------------------------------------------- */

char *strndup(const char *s, size_t n)
{
    size_t len = strnlen(s, n);
    char *d = malloc(len + 1);
    if (!d) return NULL;
    memcpy(d, s, len);
    d[len] = 0;
    return d;
}

void *memmem(const void *hay, size_t hlen, const void *needle, size_t nlen)
{
    if (!nlen) return (void *)hay;
    const unsigned char *h = hay, *end = h + hlen, *n = needle;
    while (hlen >= nlen) {
        const unsigned char *p = memchr(h, n[0], hlen - nlen + 1);
        if (!p) return NULL;
        if (!memcmp(p, n, nlen)) return (void *)p;
        hlen -= (size_t)(p + 1 - h);
        h = p + 1;
        (void)end;
    }
    return NULL;
}

/* ---- files ------------------------------------------------------------- */

int posix_rename(const char *from, const char *to)
{
    if (MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) return 0;
    errno = GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND ? ENOENT : EACCES;
    return -1;
}

char *realpath(const char *path, char *out)
{
    char *r = _fullpath(out, path, PATH_MAX);
    if (!r) return NULL;
    if (_access(r, 0) != 0) { errno = ENOENT; return NULL; }
    return r;
}

struct posix_dir {
    HANDLE h;
    WIN32_FIND_DATAA fd;
    bool first;
    struct dirent ent;
};

DIR *opendir(const char *path)
{
    char pattern[MAX_PATH + 4];
    int n = snprintf(pattern, sizeof pattern, "%s\\*", path);
    if (n < 0 || n >= (int)sizeof pattern) { errno = ENAMETOOLONG; return NULL; }
    DIR *d = calloc(1, sizeof *d);
    if (!d) { errno = ENOMEM; return NULL; }
    d->h = FindFirstFileA(pattern, &d->fd);
    if (d->h == INVALID_HANDLE_VALUE) {
        free(d);
        errno = ENOENT;
        return NULL;
    }
    d->first = true;
    return d;
}

struct dirent *readdir(DIR *d)
{
    if (!d->first && !FindNextFileA(d->h, &d->fd)) return NULL;
    d->first = false;
    snprintf(d->ent.d_name, sizeof d->ent.d_name, "%s", d->fd.cFileName);
    d->ent.d_type = (d->fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? DT_DIR : DT_REG;
    return &d->ent;
}

int closedir(DIR *d)
{
    if (!d) return -1;
    FindClose(d->h);
    free(d);
    return 0;
}

/* ---- environment ------------------------------------------------------- */

int setenv(const char *name, const char *value, int overwrite)
{
    if (!overwrite && getenv(name)) return 0;
    return _putenv_s(name, value) == 0 ? 0 : -1;
}

int unsetenv(const char *name) { return _putenv_s(name, "") == 0 ? 0 : -1; }

/* ---- time -------------------------------------------------------------- */

/* 100 ns intervals between 1601-01-01 (FILETIME) and 1970-01-01. */
#define EPOCH_DIFF 116444736000000000ULL

int clock_gettime(clockid_t clock, struct timespec *ts)
{
    if (clock == CLOCK_MONOTONIC) {
        static LARGE_INTEGER freq;
        if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
        LARGE_INTEGER c;
        QueryPerformanceCounter(&c);
        ts->tv_sec = (time_t)(c.QuadPart / freq.QuadPart);
        ts->tv_nsec = (long)((c.QuadPart % freq.QuadPart) * 1000000000LL / freq.QuadPart);
        return 0;
    }
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    unsigned long long t = ((unsigned long long)ft.dwHighDateTime << 32 | ft.dwLowDateTime) - EPOCH_DIFF;
    ts->tv_sec = (time_t)(t / 10000000ULL);
    ts->tv_nsec = (long)(t % 10000000ULL) * 100;
    return 0;
}

int gettimeofday(struct timeval *tv, void *tz)
{
    (void)tz;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    tv->tv_sec = (long)ts.tv_sec;
    tv->tv_usec = ts.tv_nsec / 1000;
    return 0;
}

int settimeofday(const struct timeval *tv, const void *tz)
{
    (void)tv;
    (void)tz;
    errno = EPERM;              /* only the simulated clock is ever set (sim_wallclock.h) */
    return -1;
}

/* gmtime_r and timegm for any year, as newlib's: the CRT's gmtime_s and
   _mkgmtime take only 1970-3000. Proleptic Gregorian calendar, days from
   1970-01-01 (H. Hinnant's civil_from_days / days_from_civil). */
static long long floor_div(long long a, long long b) { return a / b - (a % b != 0 && (a < 0) != (b < 0)); }

static long long days_from_civil(long long y, int m, int d)
{
    y -= m <= 2;
    long long era = floor_div(y, 400);
    long long yoe = y - era * 400;
    long long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    long long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

struct tm *gmtime_r(const time_t *t, struct tm *out)
{
    long long days = floor_div(*t, 86400), secs = *t - days * 86400;
    long long z = days + 719468, era = floor_div(z, 146097);
    long long doe = z - era * 146097;
    long long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    long long mp = (5 * doy + 2) / 153;
    int m = (int)(mp < 10 ? mp + 3 : mp - 9);
    long long y = yoe + era * 400 + (m <= 2);
    *out = (struct tm){ 0 };
    out->tm_year = (int)(y - 1900);
    out->tm_mon = m - 1;
    out->tm_mday = (int)(doy - (153 * mp + 2) / 5 + 1);
    out->tm_hour = (int)(secs / 3600);
    out->tm_min = (int)(secs / 60 % 60);
    out->tm_sec = (int)(secs % 60);
    out->tm_wday = (int)((days % 7 + 11) % 7);          /* 1970-01-01 was a Thursday */
    out->tm_yday = (int)(days - days_from_civil(y, 1, 1));
    return out;
}

time_t timegm(struct tm *tm)
{
    /* Normalises the fields (month 13, day 0, ...) as mktime does. */
    long long mon = tm->tm_mon, y = tm->tm_year + 1900LL + floor_div(mon, 12);
    mon -= floor_div(mon, 12) * 12;
    long long t = (days_from_civil(y, (int)mon + 1, 1) + tm->tm_mday - 1) * 86400
                + tm->tm_hour * 3600LL + tm->tm_min * 60LL + tm->tm_sec;
    time_t r = (time_t)t;
    gmtime_r(&r, tm);
    return r;
}

int usleep(useconds_t us)
{
    Sleep((us + 999) / 1000);
    return 0;
}

unsigned sleep(unsigned seconds)
{
    Sleep(seconds * 1000);
    return 0;
}

/* ---- randomness -------------------------------------------------------- */

void arc4random_buf(void *buf, size_t n)
{
    unsigned char *p = buf;
    while (n) {
        ULONG chunk = n > 0x10000000 ? 0x10000000 : (ULONG)n;
        if (BCryptGenRandom(NULL, p, chunk, BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) abort();
        p += chunk;
        n -= chunk;
    }
}

uint32_t arc4random(void)
{
    uint32_t v;
    arc4random_buf(&v, sizeof v);
    return v;
}
