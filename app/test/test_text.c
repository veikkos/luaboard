#include "check.h"
#include "text.h"

#include <string.h>

void test_text(int *total, int *failed);

/* text_mark_cut on `s` in a buffer of `cap` bytes. */
static const char *cut(const char *s, size_t cap)
{
    static char buf[64];
    snprintf(buf, sizeof buf, "%s", s);
    text_mark_cut(buf, cap);
    return buf;
}

void test_text(int *total, int *failed)
{
    CHECK(!strcmp(cut("abcdefgh", 8), "abcd\xE2\x80\xA6"));               /* 4 + 3 bytes and the NUL */
    CHECK(!strcmp(cut("abc", 8), "abc\xE2\x80\xA6"));                     /* room already */
    CHECK(!strcmp(cut("abc\xC3\xA4xyz", 8), "abc\xE2\x80\xA6"));           /* not half an ä */
    CHECK(!strcmp(cut("ab   cdef", 8), "ab\xE2\x80\xA6"));                 /* no spaces before the mark */
    CHECK(!strcmp(cut("abcdef", 4), "\xE2\x80\xA6"));
    CHECK(!strcmp(cut("abcdef", 3), "abcdef"));                           /* no room for the mark */

    *total += check_total;
    *failed += check_failed;
}
