/* TLS client sessions (mbedTLS) over non-blocking I/O the caller gives
   (conn.c: a socket, or a connection through the USB tether). The caller
   allocates the tls_t (calloc). */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"

typedef struct {
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_x509_crt ca;   /* a custom CA (opts.ca), else the bundle is used */
} tls_t;

/** Set up the session; the handshake runs later, step by step. send and
    recv move the encrypted bytes, mbedTLS style: bytes moved, 0 from recv at
    the end, MBEDTLS_ERR_SSL_WANT_WRITE/WANT_READ for none now, else an
    error. ctx is passed to them. NULL if ok, else why not. */
const char *tls_setup(tls_t *t, mbedtls_ssl_send_t *send, mbedtls_ssl_recv_t *recv, void *ctx, const char *host,
                      const char *ca_pem, size_t ca_len);
/** One step of the handshake: 0 done, MBEDTLS_ERR_SSL_WANT_READ/WRITE call
    again, else an error. */
int tls_handshake_step(tls_t *t);
/** The text for a failed call: "TLS: certificate not trusted (...)" or
    "TLS <what> failed (-0x....)". t may be NULL. */
void tls_describe(tls_t *t, const char *what, int rc, char *out, size_t cap);
/** Free the members, not t itself. close_notify: say goodbye first. */
void tls_free(tls_t *t, bool close_notify);
