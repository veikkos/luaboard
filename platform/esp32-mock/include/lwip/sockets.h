/*
 * ESP32 mock platform - <lwip/sockets.h>.
 * lwIP offers the BSD socket API on the device; on the host the real sockets
 * are used. bind() goes through the mock so the simulator can map the
 * firmware's fixed port (e.g. 3333) to another one (several simulators).
 *
 * On Windows the BSD names map onto Winsock (platform/host/win32/lwip_win32.c),
 * as lwIP maps them onto its own functions on the device: sockets are ints,
 * errors land in errno (EWOULDBLOCK, EINPROGRESS, ...), fcntl() switches
 * O_NONBLOCK and close() closes sockets too.
 */
#pragma once

#ifdef _WIN32
#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#ifndef MSG_DONTWAIT
#define MSG_DONTWAIT 0x40000000   /* dropped: the firmware's sockets are non-blocking already */
#endif
#ifndef O_NONBLOCK
#define O_NONBLOCK 0x4000
#endif
#define F_GETFL 3
#define F_SETFL 4
typedef unsigned long nfds_t;

int mock_lwip_socket(int domain, int type, int protocol);
int mock_lwip_close(int s);
int mock_lwip_fcntl(int s, int cmd, int arg);
ssize_t mock_lwip_send(int s, const void *buf, size_t len, int flags);
ssize_t mock_lwip_recv(int s, void *buf, size_t len, int flags);
int mock_lwip_accept(int s, struct sockaddr *addr, socklen_t *len);
int mock_lwip_listen(int s, int backlog);
int mock_lwip_setsockopt(int s, int level, int name, const void *val, socklen_t len);
int mock_lwip_getsockopt(int s, int level, int name, void *val, socklen_t *len);
int mock_lwip_getsockname(int s, struct sockaddr *addr, socklen_t *len);
int mock_lwip_poll(struct pollfd *fds, nfds_t n, int timeout_ms);
/* Winsock's own, for the mock's implementation (bind/connect port mapping). */
int mock_win_bind(int s, const struct sockaddr *name, socklen_t len);
int mock_win_connect(int s, const struct sockaddr *name, socklen_t len);

#define socket(d, t, p) mock_lwip_socket((d), (t), (p))
#define close(s) mock_lwip_close(s)
#define fcntl(s, cmd, arg) mock_lwip_fcntl((s), (cmd), (arg))
#define send(s, b, n, f) mock_lwip_send((s), (b), (n), (f))
#define recv(s, b, n, f) mock_lwip_recv((s), (b), (n), (f))
#define accept(s, a, l) mock_lwip_accept((s), (a), (l))
#define listen(s, b) mock_lwip_listen((s), (b))
#define setsockopt(s, l, n, v, len) mock_lwip_setsockopt((s), (l), (n), (v), (len))
#define getsockopt(s, l, n, v, len) mock_lwip_getsockopt((s), (l), (n), (v), (len))
#define getsockname(s, a, l) mock_lwip_getsockname((s), (a), (l))
#define poll(f, n, t) mock_lwip_poll((f), (n), (t))

#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

/* The host's own, seen by the simulated clock: a fast-forward waits for the
   answer to what the firmware sent on a connection it made (mock_core.c). */
ssize_t mock_lwip_send(int s, const void *buf, size_t len, int flags);
ssize_t mock_lwip_recv(int s, void *buf, size_t len, int flags);
int mock_lwip_accept(int s, struct sockaddr *addr, socklen_t *len);
#define send(s, b, n, f) mock_lwip_send((s), (b), (n), (f))
#define recv(s, b, n, f) mock_lwip_recv((s), (b), (n), (f))
#define accept(s, a, l) mock_lwip_accept((s), (a), (l))
#endif

int mock_lwip_bind(int s, const struct sockaddr *name, socklen_t namelen);
#define bind(s, name, namelen) mock_lwip_bind((s), (name), (namelen))
/* connect() to this host's mapped port reaches the simulated device, as it
   would on the chip. */
int mock_lwip_connect(int s, const struct sockaddr *name, socklen_t namelen);
#define connect(s, name, namelen) mock_lwip_connect((s), (name), (namelen))
