#include "check.h"
#include "update_manifest.h"

#include <stdio.h>
#include <string.h>

#define HEX64 "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789"

/* A manifest with the usual fields; `before` and `after` splice in text. */
#define MANIFEST(fmt, ver, sha, url, size, api, apimin, extra)                       \
    "{\"format\": " fmt ", \"version\": " ver ", \"board\": \"m5stack-cores3\","     \
    " \"url\": " url ", \"size\": " size ", \"sha256\": " sha ","                    \
    " \"lua_api\": " api ", \"lua_api_min\": " apimin extra "}"

#define URL_OK "\"https://github.com/veikkos/luaboard/releases/download/v1.1.0/luaboard-m5stack-cores3-update.bin\""
#define GOOD(extra) MANIFEST("1", "\"v1.1.0\"", "\"" HEX64 "\"", URL_OK, "1580000", "2", "1", extra)

static bool parse(const char *json, update_manifest_t *m, char *err, size_t cap)
{
    return update_manifest_parse(json, strlen(json), m, err, cap);
}

static void expect_fail(const char *json)
{
    update_manifest_t m;
    char err[96] = "";
    CHECK(!parse(json, &m, err, sizeof err));
    CHECK(err[0] != 0);
}

static void test_newer(void)
{
    CHECK(update_is_newer("v1.1.0", "v1.0.0"));
    CHECK(!update_is_newer("v1.0.0", "v1.0.0"));
    CHECK(!update_is_newer("v0.9.0", "v1.0.0"));
    CHECK(update_is_newer("v1.10.0", "v1.9.0"));
    CHECK(!update_is_newer("v1.0.0", "v1.0.0-5-gabc123"));   /* dev build after the tag */
    CHECK(update_is_newer("v1.0.1", "v1.0.0-5-gabc123"));
    CHECK(!update_is_newer("v1.0.0", "v1.0.0-5-gabc123-dirty"));
    CHECK(!update_is_newer("v1.0.0", "v1.0.0-dirty"));
    CHECK(update_is_newer("v1.0.0", "v1.0.0-rc1"));          /* a prerelease is older than its release */
    CHECK(!update_is_newer("v2.0.0", "8008651"));            /* not a tag: never offered */
    CHECK(!update_is_newer("v2.0.0", "8008651-dirty"));
    CHECK(!update_is_newer("garbage", "v1.0.0"));
    CHECK(!update_is_newer("v1.1", "v1.0.0"));
}

static void test_full(void)
{
    update_manifest_t m;
    char err[96] = "";
    CHECK(parse(GOOD(", \"notes\": \"One line for the device screen\""), &m, err, sizeof err));
    CHECK(m.format == 1);
    CHECK(strcmp(m.version, "v1.1.0") == 0);
    CHECK(strcmp(m.board, "m5stack-cores3") == 0);
    CHECK(strcmp(m.url, "https://github.com/veikkos/luaboard/releases/download/v1.1.0/luaboard-m5stack-cores3-update.bin") == 0);
    CHECK(m.size == 1580000);
    CHECK(m.sha256[0] == 0xab && m.sha256[1] == 0xcd && m.sha256[31] == 0x89);
    CHECK(m.lua_api == 2 && m.lua_api_min == 1);
    CHECK(strcmp(m.notes, "One line for the device screen") == 0);
}

static void test_unknown_fields(void)
{
    update_manifest_t m;
    char err[96] = "";
    CHECK(parse(GOOD(", \"extra\": {\"a\": [1, {\"b\": \"}\"}], \"c\": null}, \"flag\": true"), &m, err, sizeof err));
    CHECK(m.format == 1 && m.size == 1580000);
}

static void test_no_notes(void)
{
    update_manifest_t m;
    char err[96] = "";
    memset(&m, 0x55, sizeof m);
    CHECK(parse(GOOD(""), &m, err, sizeof err));
    CHECK(m.notes[0] == 0);
}

static void test_escapes(void)
{
    update_manifest_t m;
    char err[96] = "";
    CHECK(parse(GOOD(", \"notes\": \"a \\\"q\\\" \\\\ \\u00e4 \\/\""), &m, err, sizeof err));
    CHECK(strcmp(m.notes, "a \"q\" \\ \xC3\xA4 /") == 0);
    CHECK(parse(GOOD(", \"notes\": \"a \\\"q\\\" \\\\ \xC3\xA4\""), &m, err, sizeof err));
    CHECK(strcmp(m.notes, "a \"q\" \\ \xC3\xA4") == 0);
}

static void test_long_notes(void)
{
    update_manifest_t m;
    char err[96] = "";
    char json[1024];
    size_t n = (size_t)snprintf(json, sizeof json, "%s", "{\"format\": 1, \"version\": \"v1.1.0\", \"board\": \"b\","
        " \"url\": " URL_OK ", \"size\": 5, \"sha256\": \"" HEX64 "\", \"lua_api\": 2,"
        " \"lua_api_min\": 1, \"notes\": \"");
    for (int i = 0; i < 300; i++) {
        json[n++] = (char)0xC3;
        json[n++] = (char)0xA4;
    }
    strcpy(json + n, "\"}");
    CHECK(parse(json, &m, err, sizeof err));
    size_t len = strlen(m.notes);
    CHECK(len > 0 && len < UPDATE_NOTES_MAX);
    CHECK(len % 2 == 0);   /* whole characters only */
    CHECK((unsigned char)m.notes[len - 2] == 0xC3 && (unsigned char)m.notes[len - 1] == 0xA4);
}

/* Notes cut at a character that doesn't fit end there: a shorter one after
   it isn't taken instead. */
static void test_cut_notes(void)
{
    update_manifest_t m;
    char err[96] = "";
    char json[1024];
    size_t n = (size_t)snprintf(json, sizeof json, "%s", "{\"format\": 1, \"version\": \"v1.1.0\", \"board\": \"b\","
        " \"url\": " URL_OK ", \"size\": 5, \"sha256\": \"" HEX64 "\", \"lua_api\": 2,"
        " \"lua_api_min\": 1, \"notes\": \"");
    memset(json + n, 'a', UPDATE_NOTES_MAX - 2);
    n += UPDATE_NOTES_MAX - 2;
    strcpy(json + n, "\xC3\xA4" "b\"}");            /* two bytes don't fit, one would */
    CHECK(parse(json, &m, err, sizeof err));
    CHECK(strlen(m.notes) == UPDATE_NOTES_MAX - 2);
    CHECK(m.notes[UPDATE_NOTES_MAX - 3] == 'a');
    strcpy(json + n, "\\u00e4" "b\"}");             /* the same, escaped */
    CHECK(parse(json, &m, err, sizeof err));
    CHECK(strlen(m.notes) == UPDATE_NOTES_MAX - 2);
}

/* \u pairs for a character past U+FFFF make one character; half a pair
   is no character at all. */
static void test_surrogates(void)
{
    update_manifest_t m;
    char err[96] = "";
    CHECK(parse(GOOD(", \"notes\": \"a \\ud83d\\ude00 b\""), &m, err, sizeof err));
    CHECK(strcmp(m.notes, "a \xF0\x9F\x98\x80 b") == 0);
    CHECK(parse(GOOD(", \"notes\": \"a \\ud83d b \\ude00 \\ud83d\\u0041\""), &m, err, sizeof err));
    CHECK(strcmp(m.notes, "a ? b ? ?A") == 0);
}

/* A manifest of a later format is refused and its format told, whatever
   the rest looks like (format is the first field). */
static void test_later_format(void)
{
    update_manifest_t m;
    char err[96] = "";
    CHECK(!parse("{\"format\": 2, \"release\": {\"id\": [1, 2]}, \"size\": \"big\"}", &m, err, sizeof err));
    CHECK(m.format == 2 && strstr(err, "format") != NULL);
    CHECK(!parse("{\"format\": 3}", &m, err, sizeof err));
    CHECK(m.format == 3);
    /* Not a number, or after something unreadable: no format is known. */
    CHECK(!parse("{\"size\": 7, \"format\": \"x\"}", &m, err, sizeof err));
    CHECK(m.format == 0);
    CHECK(!parse("{\"size\": \"big\", \"format\": 2}", &m, err, sizeof err));
    CHECK(m.format == 0);
}

static void test_rejects(void)
{
    char err[96] = "";
    update_manifest_t m;

    CHECK(!parse(MANIFEST("2", "\"v1.1.0\"", "\"" HEX64 "\"", URL_OK, "1580000", "2", "1", ""),
                 &m, err, sizeof err));
    CHECK(strstr(err, "format") != NULL);

    expect_fail("{\"format\": 1, \"version\": \"v1.1.0\", \"board\": \"b\", \"url\": " URL_OK
                ", \"size\": 5, \"lua_api\": 2, \"lua_api_min\": 1}");   /* no sha256 */
    expect_fail(MANIFEST("1", "\"v1.1.0\"",
                         "\"abcdefg123456789abcdef0123456789abcdef0123456789abcdef0123456789\"",
                         URL_OK, "1580000", "2", "1", ""));
    expect_fail(MANIFEST("1", "\"v1.1.0\"", "\"" HEX64 "\"",
                         "\"http://example.com/luaboard-m5stack-cores3-update.bin\"", "1580000", "2", "1", ""));
    expect_fail(MANIFEST("1", "\"v1.1.0\"", "\"" HEX64 "\"", URL_OK, "0", "2", "1", ""));
    expect_fail(MANIFEST("1", "\"v1.1.0\"", "\"" HEX64 "\"", URL_OK, "1580000", "2", "3", ""));
    expect_fail("{\"format\": 1, \"board\": \"b\", \"url\": " URL_OK ", \"size\": 5, \"sha256\": \""
                HEX64 "\", \"lua_api\": 2, \"lua_api_min\": 1}");        /* no version */

    const char *good = GOOD("");
    CHECK(!update_manifest_parse(good, 40, &m, err, sizeof err));        /* cut short */
    CHECK(err[0] != 0);
    expect_fail("[1]");
}

/* The fixture's release notes (notes.md): relative links point into the
   repository at the tag, the others are as written. */
static void check_release_notes(const char *notes)
{
    static char md[4096];
    FILE *f = fopen(notes, "rb");
    size_t len = f ? fread(md, 1, sizeof md - 1, f) : 0;
    if (f) fclose(f);
    md[len] = 0;
    CHECK(len > 0);
    CHECK(strncmp(md, "The first release", 17) == 0);       /* without its heading */
    CHECK(strstr(md, "## 0.9.0") == NULL);
    CHECK(strstr(md, "[M5Stack CoreS3](https://m5stack.com/cores3)") != NULL);
    CHECK(strstr(md, "[the device guide](https://github.com/veikkos/luaboard/blob/v1.0.0/docs/DEVICE.md#updates)") != NULL);
    CHECK(strstr(md, "[the API](https://github.com/veikkos/luaboard/blob/v1.0.0/docs/LUA_API.md)") != NULL);
    CHECK(strstr(md, "[the license](https://github.com/veikkos/luaboard/blob/v1.0.0/LICENSE)") != NULL);
    CHECK(strstr(md, "[below](#older)") != NULL);
    CHECK(strstr(md, "[mail](mailto:someone@example.com)") != NULL);
    CHECK(strstr(md, "blob/v1.0.0/https") == NULL && strstr(md, "blob/v1.0.0/#") == NULL);
}

/* fw-unit --manifest <manifest-<board>.json> <image> [<notes.md>]: what
   tools/release.cmake wrote for the CHANGELOG fixture parses, and says what
   the image is. */
static int check_release_manifest(const char *manifest, const char *image, const char *notes)
{
    static char json[4096];
    FILE *f = fopen(manifest, "rb");
    if (!f) {
        printf("fw-unit: can't read %s\n", manifest);
        return 1;
    }
    size_t len = fread(json, 1, sizeof json, f);
    fclose(f);
    f = fopen(image, "rb");
    if (!f) {
        printf("fw-unit: can't read %s\n", image);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fclose(f);

    update_manifest_t m;
    char err[96] = "";
    CHECK(update_manifest_parse(json, len, &m, err, sizeof err));
    if (check_failed) printf("fw-unit: manifest: %s\n", err);
    CHECK(strcmp(m.version, "v1.0.0") == 0);
    CHECK(strcmp(m.board, "m5stack-cores3") == 0);
    CHECK(strcmp(m.url, "https://github.com/veikkos/luaboard/releases/download/v1.0.0/luaboard-m5stack-cores3-update.bin") == 0);
    CHECK((long)m.size == size);
    CHECK(m.lua_api >= m.lua_api_min && m.lua_api_min >= 1);
    CHECK(m.notes[0] != 0);
    CHECK(strlen(m.notes) <= 120);
    CHECK(strchr(m.notes, '`') == NULL);
    CHECK(strstr(m.notes, "](") == NULL);
    CHECK(strstr(m.notes, "M5Stack CoreS3") != NULL);       /* the link kept its text */
    CHECK(strstr(m.notes, "\"quoted\"") != NULL);            /* escapes came through */
    CHECK(strstr(m.notes, "back\\slash") != NULL);
    /* The fixture's paragraph runs past 120 bytes: cut at a word, marked. */
    size_t n = strlen(m.notes);
    CHECK(n >= 2 && strcmp(m.notes + n - 2, "..") == 0 && m.notes[n - 3] != ' ');
    if (notes) check_release_notes(notes);
    if (check_failed) {
        printf("fw-unit: release manifest: %d of %d checks FAILED\n", check_failed, check_total);
        return 1;
    }
    printf("fw-unit: release manifest ok (%d checks)\n", check_total);
    return 0;
}

void test_wake_time(int *total, int *failed);   /* test_wake_time.c */
void test_json(int *total, int *failed);        /* test_json.c */
void test_semver(int *total, int *failed);      /* test_semver.c */
void test_apps_index(int *total, int *failed);  /* test_apps_index.c */
void test_check_sched(int *total, int *failed); /* test_check_sched.c */
void test_qr(int *total, int *failed);          /* test_qr.c */
void test_tz(int *total, int *failed);          /* test_tz.c */
void test_text(int *total, int *failed);        /* test_text.c */
int check_marketplace(const char *rules_path, const char *index_path);   /* test_apps_index.c */

int main(int argc, char **argv)
{
    if (argc == 4 && !strcmp(argv[1], "--marketplace")) return check_marketplace(argv[2], argv[3]);
    if ((argc == 4 || argc == 5) && !strcmp(argv[1], "--manifest"))
        return check_release_manifest(argv[2], argv[3], argc == 5 ? argv[4] : NULL);
    test_newer();
    test_full();
    test_unknown_fields();
    test_no_notes();
    test_escapes();
    test_long_notes();
    test_cut_notes();
    test_later_format();
    test_surrogates();
    test_rejects();
    test_wake_time(&check_total, &check_failed);
    test_json(&check_total, &check_failed);
    test_semver(&check_total, &check_failed);
    test_apps_index(&check_total, &check_failed);
    test_check_sched(&check_total, &check_failed);
    test_qr(&check_total, &check_failed);
    test_tz(&check_total, &check_failed);
    test_text(&check_total, &check_failed);
    if (check_failed) {
        printf("fw-unit: %d of %d checks FAILED\n", check_failed, check_total);
        return 1;
    }
    printf("fw-unit: all %d checks passed\n", check_total);
    return 0;
}
