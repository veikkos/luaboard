#include "check.h"
#include "json.h"

#include <string.h>

void test_json(int *total, int *failed);

/* The first member's string value of the object `text`, or NULL. */
static const char *first(const char *text, char *out, size_t cap)
{
    json_t j;
    char key[8];
    json_init(&j, text, strlen(text));
    return json_member(&j, key, sizeof key) && json_string(&j, out, cap, NULL) ? out : NULL;
}

/* Whether `text` reads as an object, skipping every value. */
static bool well_formed(const char *text)
{
    json_t j;
    char key[8];
    json_init(&j, text, strlen(text));
    while (json_member(&j, key, sizeof key)) json_value(&j, NULL, 0);
    return !j.err;
}

/* The reader app.json and the release manifest share (json.c): what the
   update manifest tests don't already cover. */
void test_json(int *total, int *failed)
{
    char s[16];
    CHECK(first("{\"a\": \"x\\u00e4\\ud83d\\ude00\"}", s, sizeof s) && !strcmp(s, "x\xC3\xA4\xF0\x9F\x98\x80"));
    CHECK(first("{\"a\": \"x\xC3y\xFF\"}", s, sizeof s) && !strcmp(s, "x?y?"));             /* broken UTF-8 */
    CHECK(first("{\"a\": \"1\\n2\\t3\\u0001\"}", s, sizeof s) && !strcmp(s, "1 2 3 "));       /* control characters */
    CHECK(first("{\"a\": \"\xC3\xA4\xC3\xA4\"}", s, 4) && !strcmp(s, "\xC3\xA4"));             /* whole characters */

    CHECK(well_formed("{}") && well_formed(" { \"a\" : [1, {\"b\": \"]\"}], \"c\": null } "));
    CHECK(!well_formed("{\"a\": [}"));                  /* brackets must match */
    CHECK(!well_formed("{\"a\": 1,}"));
    CHECK(!well_formed("{\"a\" 1}"));
    CHECK(!well_formed("[1]"));
    CHECK(!well_formed("{\"a\": [[[[[[[[[[[[[[[[[[1]]]]]]]]]]]]]]]]]]}"));   /* nested too deep */

    json_t j;
    long v = 0;
    json_init(&j, "7 \"7\" 7.5", 9);
    CHECK(json_whole(&j, &v) && v == 7);
    CHECK(!json_whole(&j, &v) && !strcmp(j.err, "malformed number"));

    *total += check_total;
    *failed += check_failed;
}
