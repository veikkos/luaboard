/* App versions. See semver.h. */
#include "semver.h"

#include <stdio.h>
#include <string.h>

#define PART_MAX 999999999L

/* A whole number at *s (no sign, no leading "+"), moving *s past it. */
static bool number(const char **s, long *out)
{
    const char *p = *s;
    long n = 0;
    if (*p < '0' || *p > '9') return false;
    while (*p >= '0' && *p <= '9') {
        n = n * 10 + (*p++ - '0');
        if (n > PART_MAX) return false;
    }
    *out = n;
    *s = p;
    return true;
}

bool semver_parse(const char *s, semver_t *v)
{
    memset(v, 0, sizeof *v);
    if (!s) return false;
    long *parts[3] = { &v->major, &v->minor, &v->patch };
    for (int i = 0; i < 3; i++) {
        if (!number(&s, parts[i])) return false;
        if (*s != '.') break;
        if (i == 2) return false;               /* a fourth part */
        s++;
    }
    if (*s == '-') {
        const char *end = strchr(s, '+');
        size_t n = end ? (size_t)(end - s - 1) : strlen(s + 1);
        if (n == 0 || n >= sizeof v->pre) return false;
        memcpy(v->pre, s + 1, n);
        s += 1 + n;
    }
    return *s == 0 || *s == '+';
}

static int sign(long d) { return d < 0 ? -1 : d > 0; }

int semver_cmp(const char *a, const char *b)
{
    semver_t x, y;
    bool ok_a = semver_parse(a, &x), ok_b = semver_parse(b, &y);
    if (!ok_a || !ok_b) return (int)ok_a - (int)ok_b;
    if (x.major != y.major) return sign(x.major - y.major);
    if (x.minor != y.minor) return sign(x.minor - y.minor);
    if (x.patch != y.patch) return sign(x.patch - y.patch);
    if (!x.pre[0] || !y.pre[0]) return (int)!x.pre[0] - (int)!y.pre[0];   /* a release is newer */
    return sign(strcmp(x.pre, y.pre));
}
