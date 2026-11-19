#include "apps_index.h"
#include "check.h"

#include <stdio.h>
#include <string.h>

#include "json.h"

void test_apps_index(int *total, int *failed);
int check_marketplace(const char *rules_path, const char *index_path);

#define SHA_A "a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1"
#define SHA_B "b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2"
#define C1 "1111111111111111111111111111111111111111"
#define C2 "2222222222222222222222222222222222222222"
#define C3 "3333333333333333333333333333333333333333"

#define FILES "[{\"path\": \"app.json\", \"size\": 10, \"sha256\": \"" SHA_A "\"}, " \
              "{\"path\": \"main.lua\", \"size\": 20, \"sha256\": \"" SHA_B "\"}]"

/* What tools/build_index.py writes, cut down: four apps, two runnable here. */
static const char *INDEX =
    "{\n \"format\": 1,\n \"apps\": {\n"
    "  \"badsha\": {\"title\": \"Bad\", \"versions\": [{\"version\": \"1.0.0\", \"lua_api\": 1, \"commit\": \"" C1 "\","
    "   \"files\": [{\"path\": \"main.lua\", \"size\": 1, \"sha256\": \"zz\"}], \"other_files\": []}]},\n"
    "  \"bad name\": {\"title\": \"X\", \"versions\": [{\"version\": \"1.0.0\", \"lua_api\": 1, \"commit\": \"" C1 "\","
    "   \"files\": " FILES ", \"other_files\": []}]},\n"
    "  \"future\": {\"title\": \"Future\", \"versions\": [{\"version\": \"1.0.0\", \"lua_api\": 2, \"commit\": \"" C3 "\","
    "   \"files\": " FILES ", \"other_files\": []}]},\n"
    "  \"hello\": {\"title\": \"Hello\", \"description\": \"A greeting\", \"author\": \"Veikko\", \"license\": \"MIT\",\n"
    "   \"needs_setup\": false,\n"
    "   \"versions\": [\n"
    "    {\"version\": \"2.0.0\", \"lua_api\": 2, \"commit\": \"" C2 "\", \"files\": " FILES ", \"other_files\": []},\n"
    "    {\"version\": \"1.0.1\", \"lua_api\": 1, \"commit\": \"" C1 "\", \"files\": " FILES ",\n"
    "     \"other_files\": [{\"path\": \"README.md\", \"size\": 5, \"sha256\": \"" SHA_A "\"}]}\n"
    "   ]},\n"
    "  \"stocks\": {\"title\": \"Stocks\", \"license\": \"Apache-2.0 WITH LLVM-exception\", \"needs_setup\": true, \"versions\": [{\"version\": \"1.0.0\", \"lua_api\": 1,"
    "   \"commit\": \"" C3 "\", \"files\": " FILES ", \"other_files\": [{\"path\": \"secrets.env.example\", \"size\": 3,"
    "   \"sha256\": \"" SHA_A "\"}]}]}\n"
    " }\n}\n";

static apps_index_t ix;

void test_apps_index(int *total, int *failed)
{
    char err[64] = "";
    size_t len = strlen(INDEX);

    CHECK(apps_index_parse(INDEX, len, 1, 1, &ix, err, sizeof err) && !ix.newer_format);
    CHECK(ix.n == 2);                                           /* hello and stocks run here */
    const apps_entry_t *h = apps_index_find(&ix, "hello"), *s = apps_index_find(&ix, "stocks");
    CHECK(h && !strcmp(h->title, "Hello") && !strcmp(h->description, "A greeting") && !strcmp(h->author, "Veikko"));
    CHECK(h && !strcmp(h->license, "MIT") && !strcmp(h->version, "1.0.1") && h->lua_api == 1 && !strcmp(h->commit, C1));
    CHECK(h && !h->needs_setup);
    CHECK(s && s->needs_setup);
    CHECK(s && !strcmp(s->license, "Apache-2.0 WITH LLVM\xE2\x80\xA6"));   /* too long: cut, with … */
    CHECK(!apps_index_find(&ix, "future"));                      /* needs Lua API 2 */
    CHECK(!apps_index_find(&ix, "badsha"));                      /* its only version is broken */
    CHECK(!apps_index_find(&ix, "bad name"));

    apps_file_t f[4];
    CHECK(h && apps_index_files(INDEX, len, h, f, 4) == 2);
    CHECK(!strcmp(f[0].path, "app.json") && f[0].size == 10 && f[0].sha256[0] == 0xa1 && f[0].sha256[31] == 0xa1);
    CHECK(!strcmp(f[1].path, "main.lua") && f[1].size == 20 && f[1].sha256[0] == 0xb2);
    CHECK(h && apps_index_files(INDEX, len, h, f, 1) == -1);     /* more than fit */

    CHECK(apps_index_parse(INDEX, len, 1, 2, &ix, err, sizeof err) && ix.n == 3);
    h = apps_index_find(&ix, "hello");
    CHECK(h && !strcmp(h->version, "2.0.0") && !strcmp(h->commit, C2) && h->lua_api == 2);
    CHECK(apps_index_find(&ix, "future") != NULL);

    const char *later = "{\"format\": 2, \"apps\": {\"x\": {\"whatever\": [1, 2]}}}";
    CHECK(apps_index_parse(later, strlen(later), 1, 1, &ix, err, sizeof err) && ix.newer_format && ix.n == 0);

    err[0] = 0;
    CHECK(!apps_index_parse(INDEX, len / 2, 1, 1, &ix, err, sizeof err) && err[0]);   /* cut short */
    const char *noformat = "{\"apps\": {}}";
    err[0] = 0;
    CHECK(!apps_index_parse(noformat, strlen(noformat), 1, 1, &ix, err, sizeof err) && err[0]);
    const char *empty = "{\"format\": 1, \"apps\": {}}";
    CHECK(apps_index_parse(empty, strlen(empty), 1, 1, &ix, err, sizeof err) && ix.n == 0 && !ix.newer_format);

    /* Versions a board can't use are skipped: a file over the board's file
       size limit, no files, or no main.lua among them. */
    const char *bad =
        "{\"format\": 1, \"apps\": {"
        "\"big\": {\"versions\": [{\"version\": \"1.0.0\", \"lua_api\": 1, \"commit\": \"" C1 "\","
        " \"files\": [{\"path\": \"main.lua\", \"size\": 1, \"sha256\": \"" SHA_A "\"},"
        " {\"path\": \"pic.jpg\", \"size\": 262145, \"sha256\": \"" SHA_A "\"}]}]},"
        "\"empty\": {\"versions\": [{\"version\": \"1.0.0\", \"lua_api\": 1, \"commit\": \"" C1 "\", \"files\": []}]},"
        "\"nomain\": {\"versions\": [{\"version\": \"1.0.0\", \"lua_api\": 1, \"commit\": \"" C1 "\","
        " \"files\": [{\"path\": \"app.json\", \"size\": 1, \"sha256\": \"" SHA_A "\"}]}]},"
        "\"ok\": {\"versions\": [{\"version\": \"1.0.0\", \"lua_api\": 1, \"commit\": \"" C1 "\","
        " \"files\": [{\"path\": \"main.lua\", \"size\": 262144, \"sha256\": \"" SHA_A "\"}]}]}"
        "}}";
    CHECK(apps_index_parse(bad, strlen(bad), 1, 1, &ix, err, sizeof err) && ix.n == 1 && apps_index_find(&ix, "ok"));

    *total += check_total;
    *failed += check_failed;
}

/* A file's bytes into buf[cap]: how many, or -1 if it can't be read. One
   that fills buf may be longer. */
static long read_file(const char *path, char *buf, size_t cap)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        printf("fw-unit: can't read %s\n", path);
        return -1;
    }
    size_t n = fread(buf, 1, cap, f);
    fclose(f);
    return (long)n;
}

/* The marketplace's limits (the "limits" of its tools/board.json, which its
   index checks hold every app to) and the most this firmware reads of
   each. A marketplace limit may be less, never more: an app past what the
   firmware reads is left out of the board's Marketplace without a word. */
static const struct { const char *name; long most; } LIMITS[] = {
    { "apps", APPS_INDEX_MAX_APPS },
    { "index_bytes", APPS_INDEX_MAX },
    { "app_name", APPS_NAME_MAX - 1 },
    { "files", APPS_INDEX_MAX_FILES },
    { "file_name", APPS_PATH_MAX - 1 },
    { "file_bytes", APPS_FILE_MAX },
    { "title_bytes", APP_TITLE_MAX },
    { "author_bytes", APP_AUTHOR_MAX },
    { "license_bytes", APP_LICENSE_MAX },
    { "description_bytes", APP_DESC_MAX },
    { "description_lines", APP_DESC_LINES },
    { "description_cols", APP_DESC_COLS },
    { "description_qr_lines", APP_DESC_QR_LINES },
    { "description_qr_cols", APP_DESC_QR_COLS },
};
#define N_LIMITS ((int)(sizeof LIMITS / sizeof LIMITS[0]))

static void check_limits(const char *text, size_t len)
{
    json_t j, limits;
    char key[32];
    bool have = false, seen[N_LIMITS] = { false };
    json_init(&j, text, len);
    while (json_member(&j, key, sizeof key)) {
        if (!strcmp(key, "limits")) have = json_object(&j, &limits);
        else json_value(&j, NULL, 0);
    }
    CHECK(!j.err && have);
    while (have && json_member(&limits, key, sizeof key)) {
        long n = -1;
        int i = 0;
        json_whole(&limits, &n);
        while (i < N_LIMITS && strcmp(key, LIMITS[i].name)) i++;
        CHECK(i < N_LIMITS && n >= 0 && n <= LIMITS[i].most);
        if (i == N_LIMITS) {
            printf("fw-unit: marketplace limit %s: not one this firmware knows (LIMITS)\n", key);
            continue;
        }
        seen[i] = true;
        if (n < 0) printf("fw-unit: marketplace limit %s isn't a whole number\n", key);
        else if (n > LIMITS[i].most) printf("fw-unit: marketplace limit %s is %ld, the firmware reads %ld\n", key, n, LIMITS[i].most);
    }
    CHECK(!have || !limits.err);
    for (int i = 0; i < N_LIMITS; i++) {
        CHECK(seen[i]);
        if (!seen[i]) printf("fw-unit: marketplace limit %s: not in its board.json\n", LIMITS[i].name);
    }
}

/* Every app of the index is one the firmware offers, whatever Lua API its
   versions are for. */
static void check_index(const char *text, size_t len)
{
    char err[64] = "", key[16], name[64];
    json_t j, apps;
    CHECK(len <= APPS_INDEX_MAX);
    bool ok = apps_index_parse(text, len, 1, 999999999, &ix, err, sizeof err) && !ix.newer_format;
    CHECK(ok);
    if (!ok) {
        printf("fw-unit: the firmware can't read the marketplace's index: %s\n", err[0] ? err : "a newer format");
        return;
    }
    int n = 0;
    json_init(&j, text, len);
    while (json_member(&j, key, sizeof key)) {
        if (strcmp(key, "apps") || !json_object(&j, &apps)) {
            json_value(&j, NULL, 0);
            continue;
        }
        while (json_member(&apps, name, sizeof name)) {
            json_value(&apps, NULL, 0);
            n++;
            bool offered = apps_index_find(&ix, name) != NULL;
            CHECK(offered);
            if (!offered) printf("fw-unit: the firmware leaves out the marketplace's app '%s'\n", name);
        }
    }
    CHECK(n > 0 && n == ix.n);
}

/* fw-unit --marketplace <board.json> <index.json>: the limits the
   marketplace's checks keep (its tools/board.json) are within what this
   firmware reads, and the firmware offers every app of its index. */
int check_marketplace(const char *rules_path, const char *index_path)
{
    static char text[APPS_INDEX_MAX + 1];
    long len = read_file(rules_path, text, sizeof text);
    if (len < 0) return 1;
    check_limits(text, (size_t)len);
    if ((len = read_file(index_path, text, sizeof text)) < 0) return 1;
    check_index(text, (size_t)len);
    if (check_failed) {
        printf("fw-unit: marketplace: %d of %d checks FAILED\n", check_failed, check_total);
        return 1;
    }
    printf("fw-unit: marketplace ok (%d checks)\n", check_total);
    return 0;
}
