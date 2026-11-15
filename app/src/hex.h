/* Hex text <-> bytes, for the protocol, the Wi-Fi link and JSON escapes. */
#pragma once

#include <stddef.h>

/** 0..15 for a hex digit (either case), else -1. */
int hex_digit(int c);

/** Write 2 * n lowercase hex digits and a NUL to `out`. */
void hex_encode(const void *data, size_t n, char *out);

/** Decode the NUL-terminated `hex` into `out` (which may be `hex` itself).
 *  Returns the byte count, or -1 for odd length, a non-hex digit or more
 *  than `cap` bytes. */
long hex_decode(const char *hex, void *out, size_t cap);
