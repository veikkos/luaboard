#include "json.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "hex.h"
#include "text.h"

#define MAX_DEPTH 16

void json_init(json_t *j, const char *text, size_t len)
{
    *j = (json_t){ .p = text, .end = text + len };
}

bool json_fail(json_t *j, const char *why)
{
    if (!j->err) j->err = why;
    return false;
}

char json_peek(json_t *j)
{
    while (j->p < j->end && (*j->p == ' ' || *j->p == '\t' || *j->p == '\n' || *j->p == '\r')) j->p++;
    return j->p < j->end ? *j->p : 0;
}

static bool expect(json_t *j, char c)
{
    if (json_peek(j) != c) return json_fail(j, "malformed JSON");
    j->p++;
    return true;
}

bool json_member(json_t *j, char *key, size_t cap)
{
    if (j->err) return false;
    if (j->members++ == 0 && !expect(j, '{')) return false;
    if (json_peek(j) == '}') {
        j->p++;
        return false;
    }
    if (j->members > 1 && !expect(j, ',')) return false;
    bool cut;
    if (!json_string(j, key, cap, &cut) || !expect(j, ':')) return false;
    if (cut) key[0] = 0;
    return true;
}

/* The four hex digits of a \u escape. */
static bool hex4(json_t *j, uint32_t *out)
{
    if (j->end - j->p < 4) return false;
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        int d = hex_digit(j->p[i]);
        if (d < 0) return false;
        v = v * 16 + (uint32_t)d;
    }
    j->p += 4;
    *out = v;
    return true;
}

bool json_string(json_t *j, char *out, size_t cap, bool *cut)
{
    size_t n = 0;
    bool full = false;
    if (!expect(j, '"')) return false;
    for (;;) {
        if (j->p >= j->end) return json_fail(j, "malformed JSON");
        unsigned char c = (unsigned char)*j->p;
        uint32_t cp;
        if (c == '"') break;
        if (c < 0x20) return json_fail(j, "malformed JSON");
        if (c != '\\') {
            cp = text_next(&j->p, j->end);
            if (cp == 0xFFFD) cp = '?';
        } else {
            if (++j->p >= j->end) return json_fail(j, "malformed JSON");
            switch (c = (unsigned char)*j->p++) {
            case '"': case '\\': case '/': cp = c; break;
            case 'b': case 'f': case 'n': case 'r': case 't': cp = ' '; break;
            case 'u':
                if (!hex4(j, &cp)) return json_fail(j, "malformed JSON");
                /* A character past U+FFFF comes as two: 😀. */
                if (cp >= 0xD800 && cp < 0xDC00 && j->end - j->p >= 6 && j->p[0] == '\\' && j->p[1] == 'u') {
                    json_t pair = *j;
                    uint32_t lo;
                    pair.p += 2;
                    if (hex4(&pair, &lo) && lo >= 0xDC00 && lo < 0xE000) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        j->p = pair.p;
                    }
                }
                if (cp >= 0xD800 && cp < 0xE000) cp = '?';
                else if (cp < 0x20) cp = ' ';
                break;
            default: return json_fail(j, "malformed JSON");
            }
        }
        char put[4];
        size_t len = (size_t)text_utf8(cp, put);
        if (out && !full && n + len < cap) {
            memcpy(out + n, put, len);
            n += len;
        } else if (out) {
            full = true;
        }
    }
    j->p++;                                             /* the closing quote */
    if (out && cap) out[n] = 0;
    if (cut) *cut = full;
    return true;
}

static bool value(json_t *j, char *out, size_t cap, int depth)
{
    char c = json_peek(j);
    if (out && cap) out[0] = 0;
    if (!c || depth > MAX_DEPTH) return json_fail(j, "malformed JSON");
    if (c == '"') return json_string(j, out, cap, NULL);
    if (c == '{' || c == '[') {
        char close = c == '{' ? '}' : ']';
        j->p++;
        if (json_peek(j) == close) {
            j->p++;
            return true;
        }
        for (;;) {
            if (c == '{' && (!json_string(j, NULL, 0, NULL) || !expect(j, ':'))) return false;
            if (!value(j, NULL, 0, depth + 1)) return false;
            if (json_peek(j) != ',') return expect(j, close);
            j->p++;
        }
    }
    /* A number, true, false or null: a run of plain characters. */
    const char *s = j->p;
    while (j->p < j->end && !strchr(",}] \t\r\n", *j->p)) j->p++;
    if (j->p == s) return json_fail(j, "malformed JSON");
    if (out && cap) {
        size_t n = (size_t)(j->p - s) < cap - 1 ? (size_t)(j->p - s) : cap - 1;
        memcpy(out, s, n);
        out[n] = 0;
    }
    return true;
}

bool json_value(json_t *j, char *out, size_t cap)
{
    return value(j, out, cap, 0);
}

bool json_whole(json_t *j, long *out)
{
    char tok[16];
    bool quoted = json_peek(j) == '"';
    if (!json_value(j, tok, sizeof tok)) return false;
    size_t n = strspn(tok, "0123456789");
    if (quoted || n == 0 || tok[n]) return json_fail(j, "malformed number");
    if (n > 9) return json_fail(j, "number too large");
    *out = strtol(tok, NULL, 10);
    return true;
}
