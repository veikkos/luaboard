/*
 * Line-based protocol over USB serial and Wi-Fi. Spec: docs/PROTOCOL.md
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PROTO_MAX_LINKS 4   /* USB serial + NET_MAX_CLIENTS Wi-Fi clients */

/* A byte link the protocol runs over. */
typedef struct {
    const char *name;
    /** Trusted links (USB = physical access) need no pairing. */
    bool trusted;
    /** Non-blocking read: bytes read, 0 if none available. */
    int  (*read)(void *ctx, uint8_t *buf, size_t cap);
    /** Write everything (may wait briefly for the peer); drops data if nobody listens. */
    void (*write)(void *ctx, const void *data, size_t len);
    void *ctx;
} proto_link_t;

void proto_init(void);
/** Register a link; returns its id. */
int  proto_add_link(const proto_link_t *link);
/** True if an untrusted link's session has logged in (and may use the device). */
bool proto_link_authed(int id);
/** An untrusted link proved it holds the pairing key (wifilink.c). */
void proto_link_login(int id);
/** Forget partial input/uploads on a link (e.g. its TCP client went away). */
void proto_link_reset(int id);
/** Process any received bytes. Non-blocking; call from the main loop. */
void proto_poll(void);
/** Send script output as one or more "LOG ..." lines. */
void proto_log(const char *text);
/** Send an asynchronous "EVT ..." line. */
void proto_eventf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
