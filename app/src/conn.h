/* A TCP connection out, with TLS if asked, in non-blocking steps: the name
   lookup (dns.c), the connect, the TLS handshake (tls.c), then sending and
   receiving. Shared by the Lua sockets (lua_net.c) and the firmware's own
   HTTP client (httpc.c). A conn_t must stay where it is while it's open
   (TLS keeps a pointer to its fd). */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "dns.h"
#include "tls.h"

typedef enum { CONN_RESOLVING, CONN_CONNECTING, CONN_HANDSHAKE, CONN_OPEN, CONN_CLOSED } conn_state_t;

typedef struct {
    conn_state_t state;
    int fd;
    tls_t *tls;                /* NULL: plain TCP */
    int dns;                   /* CONN_RESOLVING: the lookup's slot */
    uint16_t port;
    bool app;                  /* an app's: never to this device's own control port */
    char host[DNS_HOST_MAX];
    int err;                   /* errno of a failed connect, send or receive */
    char errmsg[112];          /* why it closed otherwise (lookup, TLS); before err */
} conn_t;

#define CONN_EOF  (-1)         /* the peer closed the connection */
#define CONN_FAIL (-2)         /* an error: closed, the reason in err or errmsg */

/** A closed connection, to open or adopt. */
void conn_init(conn_t *c);
/** Start connecting to host:port, with TLS if tls (trusting ca_pem, else
 *  the built-in CAs). NULL if under way, or already failed (closed, the
 *  reason in err or errmsg); else why it can't start (left closed). */
const char *conn_open(conn_t *c, const char *host, uint16_t port, bool tls, const char *ca_pem, size_t ca_len,
                      bool app);
/** A connection that came in (accept): open, plain TCP. */
void conn_adopt(conn_t *c, int fd);
/** Move the lookup, the connect or the handshake on, while state < CONN_OPEN. */
void conn_step(conn_t *c);
/** Bytes sent (0: none now) or CONN_FAIL. */
int conn_send(conn_t *c, const void *data, size_t len);
/** Bytes received (0: nothing yet), CONN_EOF or CONN_FAIL. */
int conn_recv(conn_t *c, void *buf, size_t cap);
/** Close it (a TLS session that's open says goodbye first). err and errmsg stay. */
void conn_close(conn_t *c);
