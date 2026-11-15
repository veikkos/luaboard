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

    /* Nested objects and arrays (the marketplace index) */
    const char *nest = "{\"o\": {\"a\": \"x\", \"b\": {\"c\": \"y\"}}, \"l\": [ {\"n\": 1}, {\"n\": 2}, {\"n\": 3} ], \"e\": [], \"z\": \"end\"}";
    json_t top, in, in2;
    char key[8], val[8];
    json_init(&top, nest, strlen(nest));
    CHECK(json_member(&top, key, sizeof key) && !strcmp(key, "o") && json_object(&top, &in));
    CHECK(json_member(&in, key, sizeof key) && !strcmp(key, "a") && json_string(&in, val, sizeof val, NULL) && !strcmp(val, "x"));
    CHECK(json_member(&in, key, sizeof key) && !strcmp(key, "b") && json_object(&in, &in2));
    CHECK(json_member(&in2, key, sizeof key) && !strcmp(key, "c") && json_string(&in2, val, sizeof val, NULL) && !strcmp(val, "y"));
    CHECK(!json_member(&in2, key, sizeof key) && !in2.err);
    CHECK(!json_member(&in, key, sizeof key) && !in.err);
    CHECK(json_member(&top, key, sizeof key) && !strcmp(key, "l"));
    bool first = true;
    long sum = 0, n;
    int items = 0;
    while (json_array_next(&top, &first)) {
        CHECK(json_object(&top, &in) && json_member(&in, key, sizeof key) && json_whole(&in, &n));
        sum += n;
        items++;
    }
    CHECK(items == 3 && sum == 6 && !top.err);
    CHECK(json_member(&top, key, sizeof key) && !strcmp(key, "e"));
    first = true;
    CHECK(!json_array_next(&top, &first) && !top.err);       /* empty */
    CHECK(json_member(&top, key, sizeof key) && !strcmp(key, "z") && json_string(&top, val, sizeof val, NULL));
    CHECK(!json_member(&top, key, sizeof key) && !top.err);

    json_init(&top, "[1,]", 4);
    first = true;
    while (json_array_next(&top, &first)) json_value(&top, NULL, 0);
    CHECK(top.err != NULL);                                   /* a trailing comma */
    json_init(&top, "\"s\"", 3);
    CHECK(!json_object(&top, &in) && top.err);                /* not an object */
    first = true;
    json_init(&top, "{}", 2);
    CHECK(!json_array_next(&top, &first) && top.err);         /* not an array */

    *total += check_total;
    *failed += check_failed;
}
