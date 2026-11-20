#include "b64.h"

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static const char B64URL[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

size_t b64_encoded_len(size_t n, bool url)
{
    return url ? n / 3 * 4 + (n % 3 ? n % 3 + 1 : 0) : (n + 2) / 3 * 4;
}

size_t b64_encode(const void *data, size_t n, char *out, bool url)
{
    const uint8_t *d = data;
    const char *tab = url ? B64URL : B64;
    char *o = out;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)d[i] << 16;
        if (i + 1 < n) v |= (uint32_t)d[i + 1] << 8;
        if (i + 2 < n) v |= d[i + 2];
        *o++ = tab[(v >> 18) & 63];
        *o++ = tab[(v >> 12) & 63];
        if (i + 1 < n) *o++ = tab[(v >> 6) & 63]; else if (!url) *o++ = '=';
        if (i + 2 < n) *o++ = tab[v & 63]; else if (!url) *o++ = '=';
    }
    *o = 0;
    return (size_t)(o - out);
}

static int b64val(int c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
}

long b64_decode(const char *text, size_t n, void *out, size_t cap)
{
    uint8_t *o = out;
    size_t len = 0;
    uint32_t acc = 0;
    int bits = 0;
    for (size_t i = 0; i < n; i++) {
        int c = (unsigned char)text[i];
        if (c == '=' || c == '\n' || c == '\r' || c == ' ') continue;
        int v = b64val(c);
        if (v < 0) return -1;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (len == cap) return -1;
            o[len++] = (uint8_t)((acc >> bits) & 0xFF);
        }
    }
    return (long)len;
}
