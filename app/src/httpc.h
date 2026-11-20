/*
 * A small HTTP client: one GET at a time per handle, over plain TCP or TLS
 * (the root CA bundle), stepped from the main loop so it never blocks.
 * Redirects are followed (at most HTTPC_REDIRECTS_MAX), and the body of the
 * final 200 answer is streamed to a callback as it comes.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Body bytes of the answer, in order. Return false to stop (the request
 *  then fails with "stopped"). */
typedef bool (*httpc_body_fn)(void *ctx, const uint8_t *data, size_t len);
typedef enum { HTTPC_BUSY, HTTPC_DONE, HTTPC_FAILED } httpc_status_t;
typedef struct httpc httpc_t;

/** Start a GET of `url` (http:// or https://). NULL, and why in `err`, if
 *  the URL is bad, there's no memory or no name lookup slot free. */
httpc_t *httpc_get(const char *url, httpc_body_fn on_body, void *ctx, char *err, size_t cap);
/** Move it along; call until it isn't HTTPC_BUSY. */
httpc_status_t httpc_poll(httpc_t *c);
/** The Content-Length of the final answer, -1 if unknown (chunked, until close,
 *  or not there yet). */
long httpc_length(const httpc_t *c);
/** After HTTPC_FAILED, why: "HTTP 404", "too many redirects", "timed out",
 *  "connection closed early", a TLS error, ... */
const char *httpc_error(const httpc_t *c);
/** Whether its connection goes through the USB tether (else Wi-Fi). */
bool httpc_tethered(const httpc_t *c);
/** Free it; one in progress is dropped. NULL is fine. */
void httpc_free(httpc_t *c);
/** A short reason for an httpc_error, for one screen line (at most
 *  HTTPC_SHORT_MAX characters): "server error 404", "no connection",
 *  "download cut short", ... (buf holds the ones it makes). */
#define HTTPC_SHORT_MAX 38
const char *httpc_short_error(const char *err, char *buf, size_t cap);
