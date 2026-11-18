#include "apps_index.h"
#include "check.h"

#include <string.h>

void test_apps_index(int *total, int *failed);

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
