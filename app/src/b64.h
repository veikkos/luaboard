/* Base64 text <-> bytes, for the scripts' crypto module and the USB tether. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Characters b64_encode writes for n bytes (without the NUL). */
size_t b64_encoded_len(size_t n, bool url);

/** Write n bytes as base64 and a NUL to `out` (b64_encoded_len + 1 chars):
 *  the standard alphabet with padding, or with url the URL-safe one without.
 *  Returns the length written. */
size_t b64_encode(const void *data, size_t n, char *out, bool url);

/** At most this many bytes come from n characters of base64. */
#define B64_DECODED_MAX(n) ((n) / 4 * 3 + 3)

/** Decode n characters of either alphabet into `out`; padding and white
 *  space are skipped. Returns the byte count, or -1 for another character
 *  or more than `cap` bytes. */
long b64_decode(const char *text, size_t n, void *out, size_t cap);
