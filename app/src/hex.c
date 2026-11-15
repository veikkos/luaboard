#include "hex.h"

#include <stdint.h>
#include <string.h>

int hex_digit(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

void hex_encode(const void *data, size_t n, char *out)
{
    static const char digits[] = "0123456789abcdef";
    const uint8_t *d = data;
    for (size_t i = 0; i < n; i++) {
        out[2 * i] = digits[d[i] >> 4];
        out[2 * i + 1] = digits[d[i] & 15];
    }
    out[2 * n] = 0;
}

long hex_decode(const char *hex, void *out, size_t cap)
{
    uint8_t *o = out;
    size_t n = strlen(hex);
    if (n % 2 || n / 2 > cap) return -1;
    for (size_t i = 0; i < n / 2; i++) {
        int hi = hex_digit(hex[2 * i]), lo = hex_digit(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) return -1;
        o[i] = (uint8_t)(hi << 4 | lo);
    }
    return (long)(n / 2);
}
