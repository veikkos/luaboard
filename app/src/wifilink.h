/*
 * The encrypted Wi-Fi link: one per TCP client (net.c), carrying the same
 * byte stream as USB. Spec: docs/PROTOCOL.md, "Wi-Fi link".
 *
 *   PC     -> device  "HELLO 1 <cn: 32 hex>\n"                (clear text)
 *   device -> PC      "OK HELLO 1 <key id: 16 hex> <sn: 32 hex>\n"
 *                     or "ERR <why>\n" and the connection closes
 *   then both ways    frames: length (2 bytes, big-endian: ciphertext + 16),
 *                     AES-256-GCM ciphertext of 1..4096 bytes, 16-byte tag.
 *                     Key: c2s or s2c (auth_session_keys). Nonce: 4 zero
 *                     bytes, then a 64-bit big-endian count of the frames
 *                     sent that way so far. Additional data: the length.
 *
 * The client is logged in once one of its frames decrypts: only a holder of
 * the pairing key can make one. A frame that doesn't decrypt ends the link.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WL_MAX_PLAIN 4096

typedef void (*wl_send_fn)(void *ctx, const void *data, size_t len);

typedef struct {
    enum { WL_HELLO, WL_SECURE } state;
    uint32_t krx, ktx;            /* PSA key ids, 0 = none */
    uint64_t nrx, ntx;            /* frames received / sent */
    uint8_t *in;                  /* raw bytes received, not used yet */
    size_t in_len;
    uint8_t *plain;               /* a decrypted frame, partly handed out */
    size_t plain_len, plain_off;
    bool verified;                /* a frame from the client decrypted */
} wl_t;

/** A fresh link (new connection). False if out of memory. */
bool wl_open(wl_t *w);
/** Forget everything (connection closed). */
void wl_close(wl_t *w);
/** Where to put received bytes, and how many fit (0: full until wl_read). */
uint8_t *wl_room(wl_t *w, size_t *room);
/** n bytes were put there. Handles HELLO (replying in clear text through
 *  send). False: drop the connection. */
bool wl_received(wl_t *w, size_t n, wl_send_fn send, void *ctx);
/** Decrypted bytes for the protocol: count, 0 if none yet, -1 to drop the
 *  connection (a frame that doesn't decrypt). */
int wl_read(wl_t *w, uint8_t *buf, size_t cap);
/** Encrypt and send (nothing before HELLO is done). */
void wl_write(wl_t *w, const void *data, size_t len, wl_send_fn send, void *ctx);
