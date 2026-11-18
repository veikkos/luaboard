#include "text.h"

#include <stddef.h>
#include <string.h>

static const char LATIN[] =
    "AAAAAAACEEEEIIIIDNOOOOOxOUUUUY?s" "aaaaaaaceeeeiiiidnooooo/ouuuuy?y"   /* U+00C0 */
    "AaAaAaCcCcCcCcDdDdEeEeEeEeEeGgGg" "GgGgHhHhIiIiIiIiIiIiJjKkkLlLlLlL"   /* U+0100 */
    "lLlNnNnNnnNnOoOoOoOoRrRrRrSsSsSs" "SsTtTtTtUuUuUuUuUuUuWwYyYZzZzZzs";  /* U+0140 */

uint32_t text_next(const char **p, const char *end)
{
    const unsigned char *s = (const unsigned char *)*p;
    size_t room = (size_t)(end - *p);
    int len = s[0] < 0x80 ? 1 : s[0] >= 0xC2 && s[0] < 0xE0 ? 2 : s[0] >= 0xE0 && s[0] < 0xF0 ? 3
            : s[0] >= 0xF0 && s[0] <= 0xF4 ? 4 : 0;
    if (len == 0 || (size_t)len > room) { *p += 1; return 0xFFFD; }
    uint32_t cp = len == 1 ? s[0] : s[0] & (0x7Fu >> len);
    for (int i = 1; i < len; i++) {
        if ((s[i] & 0xC0) != 0x80) { *p += 1; return 0xFFFD; }
        cp = cp << 6 | (s[i] & 0x3F);
    }
    *p += len;
    return cp;
}

const char *text_ascii(uint32_t cp, char one[2])
{
    one[1] = 0;
    one[0] = '?';
    if (cp < 0x80) one[0] = (char)cp;
    else if (cp >= 0xC0 && cp < 0x180) one[0] = LATIN[cp - 0xC0];
    else if (cp == 0xA0) one[0] = ' ';
    else if (cp == 0x2018 || cp == 0x2019) one[0] = '\'';
    else if (cp == 0x201C || cp == 0x201D) one[0] = '"';
    else if (cp == 0x2013 || cp == 0x2014) one[0] = '-';
    else if (cp == 0x2026) return "...";
    return one;
}

int text_utf8(uint32_t cp, char out[4])
{
    if ((cp >= 0xD800 && cp < 0xE000) || cp > 0x10FFFF) cp = 0xFFFD;
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | cp >> 6);
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | cp >> 12);
        out[1] = (char)(0x80 | (cp >> 6 & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | cp >> 18);
    out[1] = (char)(0x80 | (cp >> 12 & 0x3F));
    out[2] = (char)(0x80 | (cp >> 6 & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

const char *text_skip(const char *s, int n)
{
    const char *end = s + strlen(s);
    while (n-- > 0 && s < end) text_next(&s, end);
    return s;
}

void text_mark_cut(char *s, size_t cap)
{
    static const char mark[] = "\xE2\x80\xA6";      /* … */
    if (cap < sizeof mark) return;
    size_t n = strlen(s);
    while (n > 0 && n + sizeof mark > cap) {          /* whole characters off the end */
        n--;
        while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) n--;
    }
    while (n > 0 && s[n - 1] == ' ') n--;
    memcpy(s + n, mark, sizeof mark);
}
