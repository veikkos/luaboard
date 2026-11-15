#include "update_manifest.h"

#include "hex.h"
#include "json.h"

#include <stdio.h>
#include <string.h>

/* ---- versions ---- */

typedef struct {
    long major, minor, patch;
    int rank;   /* 0 prerelease, 1 the release itself, 2 builds after it */
    long ahead; /* commits after the tag, for rank 2 */
} version_t;

static const char *read_number(const char *p, long *out)
{
    if (*p < '0' || *p > '9') return NULL;
    long v = 0;
    while (*p >= '0' && *p <= '9') {
        if (v < 100000000) v = v * 10 + (*p - '0');
        p++;
    }
    *out = v;
    return p;
}

/* "vMAJ.MIN.PAT" and an optional suffix; false for anything else (a bare hash). */
static bool parse_version(const char *s, version_t *v)
{
    if (*s++ != 'v') return false;
    if (!(s = read_number(s, &v->major)) || *s++ != '.') return false;
    if (!(s = read_number(s, &v->minor)) || *s++ != '.') return false;
    if (!(s = read_number(s, &v->patch))) return false;
    v->rank = 1;
    v->ahead = 0;
    if (*s == 0 || strcmp(s, "-dirty") == 0) return true;
    if (*s != '-') return false;
    /* git describe: -<distance>-g<hex>[-dirty] */
    const char *p = read_number(s + 1, &v->ahead);
    if (p && *p == '-' && p[1] == 'g' && hex_digit(p[2]) >= 0) {
        p += 2;
        while (hex_digit(*p) >= 0) p++;
        if (*p == 0 || strcmp(p, "-dirty") == 0) {
            v->rank = 2;
            return true;
        }
    }
    v->rank = 0;   /* any other suffix: a prerelease */
    v->ahead = 0;
    return true;
}

static int compare_long(long a, long b) { return a < b ? -1 : a > b; }

static int compare_versions(const version_t *a, const version_t *b)
{
    int c;
    if ((c = compare_long(a->major, b->major))) return c;
    if ((c = compare_long(a->minor, b->minor))) return c;
    if ((c = compare_long(a->patch, b->patch))) return c;
    if ((c = compare_long(a->rank, b->rank))) return c;
    return compare_long(a->ahead, b->ahead);
}

bool update_is_newer(const char *release, const char *running)
{
    version_t r, c;
    if (!parse_version(release, &r) || r.rank != 1) return false;
    /* A release is a plain tag: no suffix at all. */
    if (strchr(release, '-')) return false;
    if (!parse_version(running, &c)) return false;
    return compare_versions(&r, &c) > 0;
}

/* ---- the manifest ---- */

static bool set_err(char *err, size_t cap, const char *msg)
{
    if (cap) snprintf(err, cap, "%s", msg);
    return false;
}

enum {
    F_FORMAT = 1 << 0, F_VERSION = 1 << 1, F_BOARD = 1 << 2, F_URL = 1 << 3,
    F_SIZE = 1 << 4, F_SHA = 1 << 5, F_API = 1 << 6, F_APIMIN = 1 << 7,
    F_ALL = 0xFF,
};

bool update_manifest_parse(const char *text, size_t len, update_manifest_t *m, char *err, size_t cap)
{
    json_t j;
    unsigned seen = 0;
    char key[24];
    bool cut;
    long v = 0;

    memset(m, 0, sizeof *m);
    if (cap) err[0] = 0;
    json_init(&j, text, len);
    if (json_peek(&j) != '{') return set_err(err, cap, "not a JSON object");
    while (json_member(&j, key, sizeof key)) {
        if (!strcmp(key, "format")) {
            seen |= F_FORMAT;
            if (json_whole(&j, &v)) m->format = (int)v;
            /* Another format: the rest isn't this firmware's to read. */
            if (m->format > 1) return set_err(err, cap, "unknown format");
        } else if (!strcmp(key, "version") || !strcmp(key, "board") || !strcmp(key, "url") ||
                   !strcmp(key, "notes")) {
            char *dst = key[0] == 'v' ? m->version : key[0] == 'b' ? m->board
                      : key[0] == 'u' ? m->url : m->notes;
            size_t dcap = key[0] == 'v' ? sizeof m->version : key[0] == 'b' ? sizeof m->board
                        : key[0] == 'u' ? sizeof m->url : sizeof m->notes;
            /* Notes are cut to fit; the others must fit whole. */
            if (json_string(&j, dst, dcap, &cut) && cut && key[0] != 'n') json_fail(&j, "a field is too long");
            if (key[0] == 'v') seen |= F_VERSION;
            else if (key[0] == 'b') seen |= F_BOARD;
            else if (key[0] == 'u') seen |= F_URL;
        } else if (!strcmp(key, "size")) {
            if (json_whole(&j, &v)) m->size = (size_t)v;
            seen |= F_SIZE;
        } else if (!strcmp(key, "lua_api")) {
            if (json_whole(&j, &v)) m->lua_api = (int)v;
            seen |= F_API;
        } else if (!strcmp(key, "lua_api_min")) {
            if (json_whole(&j, &v)) m->lua_api_min = (int)v;
            seen |= F_APIMIN;
        } else if (!strcmp(key, "sha256")) {
            char hex[72];
            if (json_string(&j, hex, sizeof hex, &cut) && (cut || hex_decode(hex, m->sha256, sizeof m->sha256) != 32))
                json_fail(&j, "sha256 is not 64 hex digits");
            seen |= F_SHA;
        } else {
            json_value(&j, NULL, 0);
        }
    }
    if (j.err) return set_err(err, cap, j.err);
    if (m->format != 1) return set_err(err, cap, "unknown format");
    if ((seen & F_ALL) != F_ALL) return set_err(err, cap, "a required field is missing");
    if (strncmp(m->url, "https://", 8) != 0) return set_err(err, cap, "url is not https");
    if (m->size == 0) return set_err(err, cap, "size is zero");
    if (m->lua_api_min > m->lua_api) return set_err(err, cap, "lua_api_min above lua_api");
    {
        version_t ver;
        if (!parse_version(m->version, &ver) || ver.rank != 1 || strchr(m->version, '-'))
            return set_err(err, cap, "version is not vX.Y.Z");
    }
    return true;
}
