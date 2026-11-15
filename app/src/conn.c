#include "conn.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_config.h"
#include "lwip/sockets.h"
#include "net.h"

void conn_init(conn_t *c)
{
    memset(c, 0, sizeof *c);
    c->fd = -1;
    c->dns = -1;
    c->state = CONN_CLOSED;
}

void conn_close(conn_t *c)
{
    if (c->dns >= 0) dns_abandon(c->dns);
    c->dns = -1;
    if (c->tls) {
        tls_free(c->tls, c->state == CONN_OPEN);
        free(c->tls);
        c->tls = NULL;
    }
    if (c->fd >= 0) close(c->fd);
    c->fd = -1;
    c->state = CONN_CLOSED;
}

static void tls_failed(conn_t *c, const char *what, int rc)
{
    tls_describe(c->tls, what, rc, c->errmsg, sizeof c->errmsg);
    conn_close(c);
}

/* This device's own control port, by any address that reaches it. Apps
   must not drive the device (install code into other apps, change its
   settings). net.c also refuses such connections when it accepts them, but
   an app could connect, send and close before that. */
static bool is_control_port(struct in_addr addr, uint16_t port)
{
    if (port != NET_TCP_PORT) return false;
    uint32_t a = ntohl(addr.s_addr);
    if (a == INADDR_ANY || (a >> 24) == 127) return true;
    struct in_addr me;
    return inet_pton(AF_INET, net_ip(), &me) == 1 && me.s_addr == addr.s_addr;
}

/* The TCP connection, once the address is known. */
static void start_connect(conn_t *c, struct in_addr addr)
{
    if (c->app && is_control_port(addr, c->port)) {
        snprintf(c->errmsg, sizeof c->errmsg, "that is this device's own control port");
        conn_close(c);
        return;
    }
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) { c->err = errno; conn_close(c); return; }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(c->port), .sin_addr = addr };
    int rc = connect(fd, (struct sockaddr *)&a, sizeof a);
    int e = errno;
    c->fd = fd;
    if (rc != 0 && e != EINPROGRESS) { c->err = e; conn_close(c); return; }
    c->state = rc == 0 ? (c->tls ? CONN_HANDSHAKE : CONN_OPEN) : CONN_CONNECTING;
}

const char *conn_open(conn_t *c, const char *host, uint16_t port, bool tls, const char *ca_pem, size_t ca_len,
                      bool app)
{
    conn_close(c);
    c->err = 0;
    c->errmsg[0] = 0;
    if (strlen(host) >= sizeof c->host) return "host name too long";
    snprintf(c->host, sizeof c->host, "%s", host);
    c->port = port;
    c->app = app;
    struct in_addr addr;
    bool numeric = inet_pton(AF_INET, host, &addr) == 1;
    if (!numeric && (c->dns = dns_start(host)) < 0) return "too many name lookups at once";
    c->state = CONN_RESOLVING;
    if (tls) {
        c->tls = calloc(1, sizeof *c->tls);
        const char *e = c->tls ? tls_setup(c->tls, &c->fd, host, ca_pem, ca_len) : "out of memory";
        if (e) { conn_close(c); return e; }
    }
    if (numeric) start_connect(c, addr);       /* else once the lookup is done (conn_step) */
    return NULL;
}

void conn_adopt(conn_t *c, int fd)
{
    conn_init(c);
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    c->fd = fd;
    c->state = CONN_OPEN;
}

void conn_step(conn_t *c)
{
    if (c->state == CONN_RESOLVING) {
        struct in_addr addr;
        int rc = dns_poll(c->dns, &addr);
        if (rc == 0) return;
        c->dns = -1;                                 /* the lookup is over, its slot freed */
        if (rc < 0) {
            snprintf(c->errmsg, sizeof c->errmsg, "cannot resolve %.96s", c->host);   /* (a long name cut) */
            conn_close(c);
            return;
        }
        start_connect(c, addr);
    }
    if (c->state == CONN_CONNECTING) {               /* connected once writable, the error in SO_ERROR */
        struct pollfd pfd = { .fd = c->fd, .events = POLLOUT };
        if (poll(&pfd, 1, 0) <= 0) return;
        int e = 0;
        socklen_t len = sizeof e;
        getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &e, &len);
        if (e) { c->err = e; conn_close(c); return; }
        c->state = c->tls ? CONN_HANDSHAKE : CONN_OPEN;
    }
    if (c->state == CONN_HANDSHAKE) {                /* (its crypto can take a while on the chip) */
        int rc = tls_handshake_step(c->tls);
        if (rc == 0) c->state = CONN_OPEN;
        else if (rc != MBEDTLS_ERR_SSL_WANT_READ && rc != MBEDTLS_ERR_SSL_WANT_WRITE) tls_failed(c, "handshake", rc);
    }
}

int conn_send(conn_t *c, const void *data, size_t len)
{
    if (c->state != CONN_OPEN) return c->state == CONN_CLOSED ? CONN_FAIL : 0;
    if (len == 0) return 0;
    if (c->tls) {
        int rc = mbedtls_ssl_write(&c->tls->ssl, data, len);
        if (rc >= 0) return rc;
        if (rc == MBEDTLS_ERR_SSL_WANT_WRITE || rc == MBEDTLS_ERR_SSL_WANT_READ) return 0;
        tls_failed(c, "send", rc);
        return CONN_FAIL;
    }
    ssize_t n = send(c->fd, data, len, 0);
    if (n >= 0) return (int)n;
    if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
    c->err = errno;
    conn_close(c);
    return CONN_FAIL;
}

int conn_recv(conn_t *c, void *buf, size_t cap)
{
    if (c->state != CONN_OPEN) return c->state == CONN_CLOSED ? CONN_FAIL : 0;
    if (c->tls) {
        int rc = mbedtls_ssl_read(&c->tls->ssl, buf, cap);
        if (rc > 0) return rc;
        if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE ||
            rc == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET)
            return 0;
        /* (an end without a close_notify, as some servers do it, mbedTLS gives as 0 too) */
        if (rc == 0 || rc == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) { conn_close(c); return CONN_EOF; }
        tls_failed(c, "receive", rc);
        return CONN_FAIL;
    }
    ssize_t n = recv(c->fd, buf, cap, MSG_DONTWAIT);
    if (n > 0) return (int)n;
    if (n == 0) { conn_close(c); return CONN_EOF; }
    if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
    c->err = errno;
    conn_close(c);
    return CONN_FAIL;
}
