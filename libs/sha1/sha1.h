/* SHA-1 (FIPS 180-4). Portable C, no allocation. For protocols that ask for
 * it (the WebSocket handshake): it no longer keeps a forger out, so nothing
 * that has to is built on it. The symbol is prefixed lb_ like SHA-256's. */
#pragma once

#include <stddef.h>
#include <stdint.h>

void lb_sha1(const void *data, size_t len, uint8_t out[20]);
