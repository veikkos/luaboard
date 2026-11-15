/* TLS client sessions (mbedTLS) on a non-blocking socket. The caller
   allocates the tls_t (calloc) and owns the fd. */
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

/** Set up the session; the handshake runs later, step by step. *fd is read
    on every send and receive, so it may be set after this call. NULL if ok,
    else why not. */
const char *tls_setup(tls_t *t, int *fd, const char *host, const char *ca_pem, size_t ca_len);
/** One step of the handshake: 0 done, MBEDTLS_ERR_SSL_WANT_READ/WRITE call
    again, else an error. */
int tls_handshake_step(tls_t *t);
/** The text for a failed call: "TLS: certificate not trusted (...)" or
    "TLS <what> failed (-0x....)". t may be NULL. */
void tls_describe(tls_t *t, const char *what, int rc, char *out, size_t cap);
/** Free the members, not t itself. close_notify: say goodbye first. */
void tls_free(tls_t *t, bool close_notify);
