/* TLS client sessions on a non-blocking socket. See tls.h. */
#include "tls.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "lwip/sockets.h"
#include "psa/crypto.h"

/* mbedTLS I/O on our non-blocking socket. */
static int bio_send(void *ctx, const unsigned char *buf, size_t len)
{
    ssize_t n = send(*(int *)ctx, buf, len, 0);
    if (n >= 0) return (int)n;
    return (errno == EAGAIN || errno == EWOULDBLOCK) ? MBEDTLS_ERR_SSL_WANT_WRITE : -0x004E; /* NET_SEND_FAILED */
}

static int bio_recv(void *ctx, unsigned char *buf, size_t len)
{
    ssize_t n = recv(*(int *)ctx, buf, len, MSG_DONTWAIT);
    if (n >= 0) return (int)n;                                  /* 0: peer closed */
    return (errno == EAGAIN || errno == EWOULDBLOCK) ? MBEDTLS_ERR_SSL_WANT_READ : -0x004C; /* NET_RECV_FAILED */
}

const char *tls_setup(tls_t *t, int *fd, const char *host, const char *ca_pem, size_t ca_len)
{
    static bool psa_ready;
    if (!psa_ready) {
        if (psa_crypto_init() != PSA_SUCCESS) return "crypto init failed";
        psa_ready = true;
    }
    mbedtls_ssl_init(&t->ssl);
    mbedtls_ssl_config_init(&t->conf);
    mbedtls_x509_crt_init(&t->ca);
    if (mbedtls_ssl_config_defaults(&t->conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT) != 0)
        return "TLS config failed";
    mbedtls_ssl_conf_authmode(&t->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    if (ca_pem) {
        /* PEM parsing wants the terminating NUL counted. */
        char *copy = malloc(ca_len + 1);
        if (!copy) return "out of memory";
        memcpy(copy, ca_pem, ca_len);
        copy[ca_len] = 0;
        int rc = mbedtls_x509_crt_parse(&t->ca, (unsigned char *)copy, ca_len + 1);
        free(copy);
        if (rc < 0) return "opts.ca is not a valid PEM certificate";
        mbedtls_ssl_conf_ca_chain(&t->conf, &t->ca, NULL);
    } else if (esp_crt_bundle_attach(&t->conf) != ESP_OK) {
        return "no root certificates available";
    }
    if (mbedtls_ssl_setup(&t->ssl, &t->conf) != 0) return "TLS setup failed";
    if (mbedtls_ssl_set_hostname(&t->ssl, host) != 0) return "bad TLS host name";
    mbedtls_ssl_set_bio(&t->ssl, fd, bio_send, bio_recv, NULL);
    return NULL;
}

int tls_handshake_step(tls_t *t)
{
    return mbedtls_ssl_handshake(&t->ssl);
}

void tls_describe(tls_t *t, const char *what, int rc, char *out, size_t cap)
{
    if (rc == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED && t) {
        char info[80];
        uint32_t flags = mbedtls_ssl_get_verify_result(&t->ssl);
        mbedtls_x509_crt_verify_info(info, sizeof info, "", flags);
        char *nl = strchr(info, '\n');
        if (nl) *nl = 0;
        snprintf(out, cap, "TLS: certificate not trusted (%.60s)", info);
    } else {
        snprintf(out, cap, "TLS %s failed (-0x%04X)", what, (unsigned)-rc);
    }
}

void tls_free(tls_t *t, bool close_notify)
{
    if (close_notify) mbedtls_ssl_close_notify(&t->ssl);   /* best effort */
    mbedtls_ssl_free(&t->ssl);
    mbedtls_ssl_config_free(&t->conf);
    mbedtls_x509_crt_free(&t->ca);
}
