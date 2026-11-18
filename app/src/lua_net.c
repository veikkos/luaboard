/*
 * Lua `net` module: non-blocking TCP sockets (client and server) on top of the
 * lwIP BSD socket API - the host's real sockets in the simulator.
 *
 * Everything is non-blocking so a script can never stall the device. The
 * coroutine helpers in the Lua prelude (task/sleep, net.wait_*) and the Lua
 * protocol libraries (mqtt.lua, ...) build sequential-looking code on top.
 *
 *   s = net.connect(host, port, [opts]) -> socket (connecting) | nil, err
 *       opts.tls = true            TLS (https, mqtts, ...): server certificate
 *                                  checked against the root CA bundle
 *       opts.ca = pem              trust this CA (PEM text) instead of the bundle
 *   s:status()                    -> "connecting" | "connected" | "closed", err
 *                                    (with TLS: "connected" after the handshake)
 *   s:send(data)                  -> bytes accepted (may be < #data) | nil, err
 *   s:recv([max])                 -> data | "" (nothing yet) | nil, "closed"
 *   s:close()
 *   s:peer()                      -> "ip:port"
 *   srv = net.listen(port)        -> server | nil, err
 *   srv:accept()                  -> socket | nil (no client waiting)
 *
 * Host names are looked up on a worker thread, so connect() never blocks: the
 * socket says "connecting" meanwhile, and "closed", "cannot resolve <host>"
 * if there is no such name.
 *
 * All sockets are tracked and closed when the VM is reset (new script run).
 */
#include "lua_net.h"

#include <stdio.h>
#include <string.h>

#include <stdlib.h>

#include "app_config.h"
#include "conn.h"
#include "esp_log.h"
#include "hw.h"
#include "lauxlib.h"
#include "net.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "script.h"

static const char *TAG = "net";

#define MT_SOCK   "luaboard.socket"
#define MAX_SOCKS 16       /* lwIP has 24 (sdkconfig.defaults): the rest are the control port's */
#define MAX_TLS   4        /* each TLS connection needs ~40 KB (PSRAM on the device) */

typedef struct {
    conn_t c;             /* a listening socket keeps its fd here too, as open */
    bool listening;
    char peer[48];
} lsock_t;

/* Every open socket, so a VM reset can close them all. */
static lsock_t *s_open[MAX_SOCKS];

static void track(lsock_t *s)
{
    for (int i = 0; i < MAX_SOCKS; i++) if (!s_open[i]) { s_open[i] = s; return; }
}

static void untrack(lsock_t *s)
{
    for (int i = 0; i < MAX_SOCKS; i++) if (s_open[i] == s) s_open[i] = NULL;
}

/* After anything that may have closed the connection: a closed socket isn't open any more. */
static void settle(lsock_t *s)
{
    if (s->c.state == CONN_CLOSED) untrack(s);
}

static int open_count(void)
{
    int n = 0;
    for (int i = 0; i < MAX_SOCKS; i++) n += s_open[i] != NULL;
    return n;
}

static int tls_count(void)
{
    int n = 0;
    for (int i = 0; i < MAX_SOCKS; i++) n += s_open[i] && s_open[i]->c.tls;
    return n;
}

static void sock_close(lsock_t *s)
{
    conn_close(&s->c);
    untrack(s);
}

void lua_net_close_all(void)
{
    for (int i = 0; i < MAX_SOCKS; i++) {
        if (s_open[i]) {
            conn_close(&s_open[i]->c);
            s_open[i] = NULL;
        }
    }
}

bool lua_net_is_own_socket(const struct sockaddr_in *peer)
{
    for (int i = 0; i < MAX_SOCKS; i++) {
        lsock_t *s = s_open[i];
        if (!s || s->c.fd < 0 || s->listening) continue;
        struct sockaddr_in me;
        socklen_t len = sizeof me;
        if (getsockname(s->c.fd, (struct sockaddr *)&me, &len) == 0 && me.sin_family == AF_INET &&
            me.sin_port == peer->sin_port && me.sin_addr.s_addr == peer->sin_addr.s_addr)
            return true;
    }
    return false;
}

static const char *state_name(const lsock_t *s)
{
    if (s->listening && s->c.state == CONN_OPEN) return "listening";
    return s->c.state < CONN_OPEN ? "connecting" : s->c.state == CONN_OPEN ? "connected" : "closed";
}

/* The Lua object comes first, before any fd exists: allocating it can throw
   (out of memory), and an fd made before that would never be closed. */
static lsock_t *new_sock(lua_State *L)
{
    lsock_t *s = lua_newuserdatauv(L, sizeof *s, 0);
    conn_init(&s->c);
    s->listening = false;
    s->peer[0] = 0;
    luaL_setmetatable(L, MT_SOCK);
    return s;
}

static lsock_t *check_sock(lua_State *L) { return luaL_checkudata(L, 1, MT_SOCK); }

static int push_err(lua_State *L, const char *what, int e)
{
    lua_pushnil(L);
    lua_pushfstring(L, "%s: %s", what, strerror(e));
    return 2;
}

/* ------------------------------------------------------------ net.* */

static int l_connect(lua_State *L)
{
    const char *host = luaL_checkstring(L, 1);
    lua_Integer port = luaL_checkinteger(L, 2);
    luaL_argcheck(L, port > 0 && port < 65536, 2, "port must be 1..65535");
    bool tls = false;
    const char *ca = NULL;
    size_t ca_len = 0;
    if (!lua_isnoneornil(L, 3)) {
        luaL_checktype(L, 3, LUA_TTABLE);
        lua_getfield(L, 3, "tls");
        if (!lua_isnil(L, -1) && !lua_isboolean(L, -1)) return luaL_error(L, "net.connect: opts.tls must be true or false");
        tls = lua_toboolean(L, -1);
        lua_getfield(L, 3, "ca");
        if (!lua_isnil(L, -1)) { ca = luaL_checklstring(L, -1, &ca_len); tls = true; }
        lua_pop(L, 2);    /* ca stays valid: the opts table still references it */
    }
    luaL_argcheck(L, strlen(host) < DNS_HOST_MAX, 1, "host name too long");
    /* Out of sockets is a state, not a wrong call: nil, err, so http can
       let go of a kept connection and mqtt can try again later. */
    if (open_count() >= MAX_SOCKS) {
        lua_pushnil(L);
        lua_pushfstring(L, "too many open sockets (max %d)", MAX_SOCKS);
        return 2;
    }
    if (tls && tls_count() >= MAX_TLS) {
        lua_pushnil(L);
        lua_pushfstring(L, "too many TLS connections (max %d)", MAX_TLS);
        return 2;
    }

    lsock_t *s = new_sock(L);
    snprintf(s->peer, sizeof s->peer, "%s:%d", host, (int)port);
    /* (an app's: never to this device's own control port) */
    const char *e = conn_open(&s->c, host, (uint16_t)port, tls, ca, ca_len, true);
    if (e) {
        lua_pushnil(L);
        lua_pushstring(L, e);
        return 2;
    }
    track(s);
    settle(s);                                    /* (it may have failed at once) */
    return 1;
}

/* net._join(t): the strings t[1..#t] as one, made in one go. table.concat
   grows a buffer by doubling and then copies it, so a big http body would
   need three to four times its size; this needs twice (the pieces and the
   result). */
static int l_join(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TTABLE);
    /* Raw length and raw reads: no app metamethods run here, so the count
       checked against the stack is the count pushed. */
    lua_Unsigned n = lua_rawlen(L, 1);
    if (n == 0) { lua_pushliteral(L, ""); return 1; }
    if (n > 100000) return luaL_error(L, "net._join: too many pieces");
    luaL_checkstack(L, (int)n, "too many pieces");
    for (lua_Unsigned i = 1; i <= n; i++) {
        if (lua_rawgeti(L, 1, (lua_Integer)i) != LUA_TSTRING) return luaL_error(L, "net._join: piece %d is not a string", (int)i);
    }
    lua_concat(L, (int)n);
    return 1;
}

/* net.wifi() -> "off" | "connecting" | "connected", ip (when connected) */
static int l_wifi(lua_State *L)
{
    const char *st = net_state();
    lua_pushstring(L, st);
    if (strcmp(st, "connected")) return 1;
    lua_pushstring(L, net_ip());
    return 2;
}

static int l_listen(lua_State *L)
{
    lua_Integer port = luaL_checkinteger(L, 1);
    luaL_argcheck(L, port > 0 && port < 65536, 1, "port must be 1..65535");
    /* The control port is the firmware's: an app holding it would pose as the
       device to the PC tools. */
    luaL_argcheck(L, port != NET_TCP_PORT, 1, "that is the device's control port");
    if (open_count() >= MAX_SOCKS) {
        lua_pushnil(L);
        lua_pushfstring(L, "too many open sockets (max %d)", MAX_SOCKS);
        return 2;
    }
    lsock_t *s = new_sock(L);
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) return push_err(L, "socket", errno);
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port),
                             .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (bind(fd, (struct sockaddr *)&a, sizeof a) != 0 || listen(fd, 4) != 0) {
        int e = errno;
        close(fd);
        return push_err(L, "listen", e);
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    s->c.fd = fd;
    s->c.state = CONN_OPEN;
    s->listening = true;
    track(s);
    snprintf(s->peer, sizeof s->peer, "*:%d", (int)port);
    return 1;
}

/* ---------------------------------------------------------- socket:* */

/* Move a connection that isn't up yet along: the lookup, the connect, the
   TLS handshake. A TLS step's crypto can take a while on the chip: not
   counted against the watchdog. */
static void update_connecting(lua_State *L, lsock_t *s)
{
    if (s->listening || s->c.state >= CONN_OPEN) return;
    bool tls = s->c.tls != NULL;
    uint32_t t0 = hw_millis();
    conn_step(&s->c);
    settle(s);
    if (tls) script_watchdog_credit(L, t0, 3000);
}

static int push_closed(lua_State *L, lsock_t *s)
{
    lua_pushnil(L);
    if (s->c.errmsg[0]) lua_pushstring(L, s->c.errmsg);
    else lua_pushliteral(L, "closed");
    return 2;
}

static int m_status(lua_State *L)
{
    lsock_t *s = check_sock(L);
    update_connecting(L, s);
    lua_pushstring(L, state_name(s));
    if (s->c.state == CONN_CLOSED && (s->c.errmsg[0] || s->c.err)) {
        lua_pushstring(L, s->c.errmsg[0] ? s->c.errmsg : strerror(s->c.err));
        return 2;
    }
    return 1;
}

static int m_send(lua_State *L)
{
    lsock_t *s = check_sock(L);
    size_t len;
    const char *data = luaL_checklstring(L, 2, &len);
    update_connecting(L, s);
    if (s->listening) return push_closed(L, s);
    if (s->c.state < CONN_OPEN) { lua_pushinteger(L, 0); return 1; }       /* still connecting */
    int n = conn_send(&s->c, data, len);
    settle(s);
    if (n < 0) return push_closed(L, s);
    lua_pushinteger(L, n);
    return 1;
}

static int m_recv(lua_State *L)
{
    lsock_t *s = check_sock(L);
    lua_Integer max = luaL_optinteger(L, 2, 1024);
    luaL_argcheck(L, max > 0 && max <= 16384, 2, "max must be 1..16384");
    update_connecting(L, s);
    if (s->listening) return push_closed(L, s);
    if (s->c.state < CONN_OPEN) { lua_pushliteral(L, ""); return 1; }      /* still connecting */
    if (s->c.state != CONN_OPEN) return push_closed(L, s);
    luaL_Buffer b;
    char *p = luaL_buffinitsize(L, &b, (size_t)max);
    int n = conn_recv(&s->c, p, (size_t)max);
    settle(s);
    luaL_pushresultsize(&b, n > 0 ? (size_t)n : 0);   /* "" : nothing yet */
    if (n >= 0) return 1;
    lua_pop(L, 1);
    return push_closed(L, s);
}

static int m_accept(lua_State *L)
{
    lsock_t *s = check_sock(L);
    if (!s->listening) return luaL_error(L, "accept() on a socket that is not listening");
    if (open_count() >= MAX_SOCKS) { lua_pushnil(L); return 1; }   /* leave it queued */
    lsock_t *c = new_sock(L);
    struct sockaddr_in peer;
    socklen_t plen = sizeof peer;
    int fd = accept(s->c.fd, (struct sockaddr *)&peer, &plen);
    if (fd < 0) { lua_pushnil(L); return 1; }
    conn_adopt(&c->c, fd);
    track(c);
    char ip[16];
    inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof ip);
    snprintf(c->peer, sizeof c->peer, "%s:%d", ip, ntohs(peer.sin_port));
    return 1;
}

static int m_close(lua_State *L)
{
    lsock_t *s = check_sock(L);
    if (s->c.state != CONN_CLOSED) sock_close(s);
    return 0;
}

static int m_peer(lua_State *L)
{
    lua_pushstring(L, check_sock(L)->peer);
    return 1;
}

static int m_gc(lua_State *L)
{
    lsock_t *s = luaL_checkudata(L, 1, MT_SOCK);
    if (s->c.state != CONN_CLOSED) {
        ESP_LOGI(TAG, "closing unreferenced socket %s", s->peer);
        sock_close(s);
    }
    return 0;
}

static int m_tostring(lua_State *L)
{
    lsock_t *s = check_sock(L);
    lua_pushfstring(L, "socket(%s%s, %s)", s->c.tls ? "tls " : "", s->peer, state_name(s));
    return 1;
}

void lua_net_register(lua_State *L)
{
    static const luaL_Reg methods[] = {
        { "status", m_status }, { "send", m_send }, { "recv", m_recv },
        { "accept", m_accept }, { "close", m_close }, { "peer", m_peer },
        { NULL, NULL },
    };
    luaL_newmetatable(L, MT_SOCK);
    luaL_newlib(L, methods);
    int idx = lua_gettop(L);
    lua_pushvalue(L, idx);
    lua_setfield(L, -3, "__index");
    lua_pushcfunction(L, m_gc);
    lua_setfield(L, -3, "__gc");
    lua_pushcfunction(L, m_gc);
    lua_setfield(L, -3, "__close");
    lua_pushcfunction(L, m_tostring);
    lua_setfield(L, -3, "__tostring");
    /* Hide the metatable from getmetatable(): a script that removed __gc
       would get the socket freed while it is still in s_open (use after free). */
    lua_pushboolean(L, 0);
    lua_setfield(L, -3, "__metatable");

    static const luaL_Reg funcs[] = {
        { "connect", l_connect }, { "listen", l_listen }, { "_join", l_join }, { "wifi", l_wifi }, { NULL, NULL },
    };
    luaL_newlib(L, funcs);
    lua_pushvalue(L, idx);
    lua_setfield(L, -2, "_methods");      /* the prelude adds the waiting ones (and removes this) */
    lua_setglobal(L, "net");
    lua_pop(L, 2);                        /* methods, metatable */
}
