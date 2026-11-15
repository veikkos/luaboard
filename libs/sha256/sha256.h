/* SHA-256 and HMAC-SHA256 (FIPS 180-4, RFC 2104). Portable C, no allocation.
 * Used for the Wi-Fi link's keys, firmware update checks and crypto.sha256/hmac.
 * Symbols are prefixed lb_: ESP-IDF's wpa_supplicant exports hmac_sha256 too. */
#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t h[8];
    uint64_t len;       /* bytes hashed so far */
    uint8_t buf[64];
    size_t nbuf;
} lb_sha256_ctx_t;

void lb_sha256_init(lb_sha256_ctx_t *c);
void lb_sha256_update(lb_sha256_ctx_t *c, const void *data, size_t len);
void lb_sha256_final(lb_sha256_ctx_t *c, uint8_t out[32]);
void lb_sha256(const void *data, size_t len, uint8_t out[32]);
void lb_hmac_sha256(const void *key, size_t keylen, const void *msg, size_t msglen, uint8_t out[32]);
