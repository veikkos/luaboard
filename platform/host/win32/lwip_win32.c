/*
 * The lwIP socket API over Winsock (esp32-mock/include/lwip/sockets.h): what
 * the firmware calls by its BSD names. Sockets are ints (Winsock's SOCKET
 * values fit), and errors are put in errno as lwIP does, so the firmware's
 * `errno == EAGAIN` and `EINPROGRESS` checks work unchanged.
 */
#include "posix/posix_compat.h"

#include "lwip/sockets.h"

#undef socket
#undef close
#undef fcntl
#undef send
#undef recv
#undef accept
#undef listen
#undef setsockopt
#undef getsockopt
#undef getsockname
#undef poll
#undef bind
#undef connect

static int errno_of(int wsa, bool connecting)
{
    switch (wsa) {
    case WSAEWOULDBLOCK: return connecting ? EINPROGRESS : EWOULDBLOCK;
    case WSAEINPROGRESS: return EINPROGRESS;
    case WSAEALREADY: return EALREADY;
    case WSAEINTR: return EINTR;
    case WSAECONNREFUSED: return ECONNREFUSED;
    case WSAECONNRESET: return ECONNRESET;
    case WSAECONNABORTED: return ECONNABORTED;
    case WSAETIMEDOUT: return ETIMEDOUT;
    case WSAEADDRINUSE: return EADDRINUSE;
    case WSAEADDRNOTAVAIL: return EADDRNOTAVAIL;
    case WSAENOTCONN: return ENOTCONN;
    case WSAEISCONN: return EISCONN;
    case WSAEHOSTUNREACH: return EHOSTUNREACH;
    case WSAENETUNREACH: return ENETUNREACH;
    case WSAENETDOWN: return ENETDOWN;
    case WSAEMFILE: return EMFILE;
    case WSAENOBUFS: return ENOBUFS;
    case WSAEINVAL: return EINVAL;
    case WSAENOTSOCK: return ENOTSOCK;
    case WSAESHUTDOWN: return EPIPE;
    case 0: return 0;
    default: return EIO;
    }
}

static int fail(bool connecting)
{
    errno = errno_of(WSAGetLastError(), connecting);
    return -1;
}

static bool is_socket(int s)
{
    int type;
    int len = sizeof type;
    return getsockopt((SOCKET)s, SOL_SOCKET, SO_TYPE, (char *)&type, &len) == 0;
}

int mock_lwip_socket(int domain, int type, int protocol)
{
    /* Not inheritable: a reboot's next process must not keep our ports. */
    SOCKET s = WSASocketW(domain, type, protocol, NULL, 0, WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
    if (s == INVALID_SOCKET) return fail(false);
    return (int)s;
}

int mock_lwip_close(int s)
{
    if (is_socket(s)) return closesocket((SOCKET)s) == 0 ? 0 : fail(false);
    return _close(s);
}

int mock_lwip_fcntl(int s, int cmd, int arg)
{
    if (cmd == F_GETFL) return 0;                     /* Winsock can't tell; callers only add O_NONBLOCK */
    if (cmd != F_SETFL) { errno = EINVAL; return -1; }
    u_long on = (arg & O_NONBLOCK) != 0;
    return ioctlsocket((SOCKET)s, FIONBIO, &on) == 0 ? 0 : fail(false);
}

ssize_t mock_lwip_send(int s, const void *buf, size_t len, int flags)
{
    int n = send((SOCKET)s, buf, (int)(len > INT_MAX ? INT_MAX : len), flags & ~MSG_DONTWAIT);
    return n == SOCKET_ERROR ? fail(false) : n;
}

ssize_t mock_lwip_recv(int s, void *buf, size_t len, int flags)
{
    int n = recv((SOCKET)s, buf, (int)(len > INT_MAX ? INT_MAX : len), flags & ~MSG_DONTWAIT);
    return n == SOCKET_ERROR ? fail(false) : n;
}

int mock_lwip_accept(int s, struct sockaddr *addr, socklen_t *len)
{
    SOCKET c = accept((SOCKET)s, addr, len);
    if (c == INVALID_SOCKET) return fail(false);
    SetHandleInformation((HANDLE)c, HANDLE_FLAG_INHERIT, 0);
    return (int)c;
}

int mock_lwip_listen(int s, int backlog) { return listen((SOCKET)s, backlog) == 0 ? 0 : fail(false); }

int mock_lwip_setsockopt(int s, int level, int name, const void *val, socklen_t len)
{
    /* SO_REUSEADDR on Windows lets another program take the port from under
       us; SO_EXCLUSIVEADDRUSE is the closest to the BSD meaning (rebinding
       right after a restart works anyway). */
    if (level == SOL_SOCKET && name == SO_REUSEADDR) return 0;
    return setsockopt((SOCKET)s, level, name, val, len) == 0 ? 0 : fail(false);
}

int mock_lwip_getsockopt(int s, int level, int name, void *val, socklen_t *len)
{
    if (getsockopt((SOCKET)s, level, name, val, len) != 0) return fail(false);
    /* A pending error comes as a Winsock code: the firmware wants errno's. */
    if (level == SOL_SOCKET && name == SO_ERROR && *len >= (socklen_t)sizeof(int))
        *(int *)val = errno_of(*(int *)val, false);
    return 0;
}

int mock_lwip_getsockname(int s, struct sockaddr *addr, socklen_t *len)
{
    return getsockname((SOCKET)s, addr, len) == 0 ? 0 : fail(false);
}

int mock_win_bind(int s, const struct sockaddr *name, socklen_t len)
{
    return bind((SOCKET)s, name, len) == 0 ? 0 : fail(false);
}

int mock_win_connect(int s, const struct sockaddr *name, socklen_t len)
{
    return connect((SOCKET)s, name, len) == 0 ? 0 : fail(true);
}

/* poll() by select(): WSAPoll doesn't report a connect that failed, select's
   except set does. */
int mock_lwip_poll(struct pollfd *fds, nfds_t n, int timeout_ms)
{
    fd_set rd, wr, ex;
    FD_ZERO(&rd);
    FD_ZERO(&wr);
    FD_ZERO(&ex);
    if (n > FD_SETSIZE) n = FD_SETSIZE;
    int any = 0;
    for (nfds_t i = 0; i < n; i++) {
        fds[i].revents = 0;
        if ((intptr_t)fds[i].fd < 0) continue;      /* (SOCKET is unsigned) */
        SOCKET s = (SOCKET)fds[i].fd;
        if (fds[i].events & POLLIN) FD_SET(s, &rd);
        if (fds[i].events & POLLOUT) FD_SET(s, &wr);
        FD_SET(s, &ex);
        any = 1;
    }
    if (!any) {
        if (timeout_ms > 0) Sleep((DWORD)timeout_ms);
        return 0;
    }
    struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
    int r = select(0, &rd, &wr, &ex, timeout_ms < 0 ? NULL : &tv);
    if (r == SOCKET_ERROR) return fail(false);
    int ready = 0;
    for (nfds_t i = 0; i < n; i++) {
        if ((intptr_t)fds[i].fd < 0) continue;      /* (SOCKET is unsigned) */
        SOCKET s = (SOCKET)fds[i].fd;
        if (FD_ISSET(s, &rd)) fds[i].revents |= POLLIN;
        if (FD_ISSET(s, &wr)) fds[i].revents |= POLLOUT;
        if (FD_ISSET(s, &ex)) fds[i].revents |= POLLERR | (fds[i].events & POLLOUT);
        if (fds[i].revents) ready++;
    }
    return ready;
}
