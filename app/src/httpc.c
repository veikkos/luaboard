/*
 * A small non-blocking HTTP/1.1 client (see httpc.h). Each httpc_poll moves
 * the request along as far as it can without waiting: the name lookup runs
 * on the DNS worker (dns.c), the socket is non-blocking and the TLS
 * handshake goes step by step (tls.c), as in lua_net.c.
 *
 * The answer's head is read a line at a time and only what's needed is kept
 * (the status, Content-Length, Transfer-Encoding, Location): GitHub's
 * redirects carry a 4 KB Content-Security-Policy line, more than is worth
 * holding.
 */
#include "httpc.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "app_config.h"
#include "conn.h"
#include "hw.h"
#include "ota.h"

#define URL_MAX        2048          /* GitHub's download redirects carry long signed queries */
#define BUF_MAX        (URL_MAX + 256) /* the request going out, then a line of the answer's head */
#define HEAD_TOTAL_MAX (32 * 1024)   /* an answer's head this long is not an HTTP server's */
#define POLL_BYTES     (16 * 1024)   /* at most this much body per httpc_poll: the main loop goes on */

typedef enum { H_CONNECT, H_SEND, H_HEAD, H_BODY, H_DONE, H_FAILED } hstate_t;
typedef enum { B_LENGTH, B_CHUNKED, B_CLOSE } bmode_t;
typedef enum { C_SIZE, C_DATA, C_DATA_END, C_TRAILER } cstate_t;

struct httpc {
    hstate_t state;
    bool https;
    char host[DNS_HOST_MAX];
    uint16_t port;
    char path[URL_MAX];        /* path and query to ask for; while the head comes, its Location */
    conn_t conn;               /* H_CONNECT: the lookup, connect and TLS handshake under way */
    int redirects;
    uint32_t last;             /* hw_millis of the last progress */
    httpc_body_fn on_body;
    void *ctx;

    char line[BUF_MAX];        /* the request, then the head's line being read */
    size_t out_len, out_off;   /* H_SEND: the request's length, how much is sent */
    size_t llen;               /* bytes in line */
    bool long_line;            /* the line didn't fit: its end was dropped */
    size_t head_total;
    int status;                /* 0 until the status line is read */
    bool chunked, have_loc, loc_too_long;
    long length;               /* Content-Length, -1 none */

    bmode_t mode;
    long left;                 /* B_LENGTH: body bytes to come; B_CHUNKED: of this chunk */
    cstate_t cstate;
    uint8_t rx[1024];
    char err[112];
};

static void progress(httpc_t *c) { c->last = hw_millis(); }

/* The connection (and a lookup) goes; the request's state stays. */

static void fail(httpc_t *c, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void fail(httpc_t *c, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(c->err, sizeof c->err, fmt, ap);
    va_end(ap);
    conn_close(&c->conn);
    c->state = H_FAILED;
}

/* The connection closed under c: fail, saying why. what: "send" or
   "receive", NULL while connecting. */
static void conn_failed(httpc_t *c, const char *what)
{
    const conn_t *k = &c->conn;
    if (k->errmsg[0]) fail(c, "%s", k->errmsg);
    else if (what) fail(c, "%s: %s", what, strerror(k->err));
    else fail(c, "cannot connect to %s: %s", c->host, strerror(k->err));
}

static void finish(httpc_t *c)
{
    conn_close(&c->conn);
    c->state = H_DONE;
}

/* Nothing that would break the request line or a header (spaces, control
   characters). */
static bool url_chars_ok(const char *s)
{
    for (; *s; s++) if ((unsigned char)*s <= ' ' || *s == 0x7f) return false;
    return true;
}

/* Take `url` (http[s]://host[:port][/path]) as where to go; it may be
   c->path itself. NULL if fine, else why not. */
static const char *set_target(httpc_t *c, const char *url)
{
    bool https;
    const char *p;
    if (!strncmp(url, "https://", 8)) { https = true; p = url + 8; }
    else if (!strncmp(url, "http://", 7)) { https = false; p = url + 7; }
    else return "not an http or https address";
    if (!url_chars_ok(url)) return "bad characters in the address";
    size_t hl = strcspn(p, ":/?#");
    if (hl == 0) return "no host in the address";
    if (hl >= DNS_HOST_MAX) return "host name too long";
    long port = https ? 443 : 80;
    const char *q = p + hl;
    if (*q == ':') {
        char *end;
        port = strtol(q + 1, &end, 10);
        if (end == q + 1 || port < 1 || port > 65535 || (*end && *end != '/' && *end != '?' && *end != '#'))
            return "bad port in the address";
        q = end;
    }
    size_t plen = strcspn(q, "#");                   /* the fragment isn't sent */
    bool slash = *q == '/';
    if (plen + (slash ? 0 : 1) >= URL_MAX) return "address too long";
    c->https = https;
    memcpy(c->host, p, hl);
    c->host[hl] = 0;
    c->port = (uint16_t)port;
    memmove(c->path + (slash ? 0 : 1), q, plen);     /* q may point into c->path */
    c->path[0] = '/';
    c->path[plen + (slash ? 0 : 1)] = 0;
    return NULL;
}

/* Begin the request to the target: the request text, and the connection
   (lookup, connect, TLS). NULL if under way, else why it can't start. */
static const char *start(httpc_t *c)
{
    c->state = H_CONNECT;
    c->status = 0;
    c->llen = 0;
    c->long_line = false;
    c->head_total = 0;
    c->chunked = c->have_loc = c->loc_too_long = false;
    c->length = -1;
    bool std_port = c->port == (c->https ? 443 : 80);
    char port[8] = "";
    if (!std_port) snprintf(port, sizeof port, ":%u", (unsigned)c->port);
    int n = snprintf(c->line, sizeof c->line,
                     "GET %s HTTP/1.1\r\nHost: %s%s\r\nUser-Agent: " FW_NAME "/%s\r\nAccept: */*\r\n"
                     "Connection: close\r\n\r\n",
                     c->path, c->host, port, fw_version());
    if (n < 0 || (size_t)n >= sizeof c->line) return "address too long";
    c->out_len = (size_t)n;
    c->out_off = 0;
    const char *e = conn_open(&c->conn, c->host, c->port, c->https, NULL, 0, NULL, false);
    if (e) return e;
    progress(c);
    if (c->conn.state == CONN_CLOSED) conn_failed(c, NULL);
    return NULL;
}

httpc_t *httpc_get(const char *url, httpc_body_fn on_body, void *ctx, char *err, size_t cap)
{
    httpc_t *c = calloc(1, sizeof *c);
    const char *e = c ? NULL : "out of memory";
    if (c) {
        conn_init(&c->conn);
        c->on_body = on_body;
        c->ctx = ctx;
        e = strlen(url) >= URL_MAX ? "address too long" : set_target(c, url);
        if (!e) e = start(c);
    }
    if (!e) return c;
    if (cap) snprintf(err, cap, "%s", e);
    httpc_free(c);
    return NULL;
}

/* ------------------------------------------------------------- I/O */

/* ------------------------------------------------------------ body */

static void deliver(httpc_t *c, const uint8_t *data, size_t len)
{
    if (len && !c->on_body(c->ctx, data, len)) fail(c, "stopped");
}

/* A line of the chunked framing is complete (in c->line). */
static void chunk_line(httpc_t *c)
{
    char *s = c->line;
    if (c->cstate == C_SIZE) {
        char *end;
        unsigned long v = strtoul(s, &end, 16);
        if (end == s || (*end && *end != ';' && *end != ' ') || v > 0x7fffffffUL) { fail(c, "bad chunked body"); return; }
        c->left = (long)v;
        c->cstate = v ? C_DATA : C_TRAILER;
    } else if (c->cstate == C_DATA_END) {
        if (*s) { fail(c, "bad chunked body"); return; }
        c->cstate = C_SIZE;
    } else if (!*s) {                                /* C_TRAILER: the empty line ends it */
        finish(c);
    }
}

static void chunked_bytes(httpc_t *c, const uint8_t *p, size_t n)
{
    while (n && c->state == H_BODY) {
        if (c->cstate == C_DATA) {
            size_t k = n < (size_t)c->left ? n : (size_t)c->left;
            deliver(c, p, k);
            p += k;
            n -= k;
            c->left -= (long)k;
            if (!c->left) { c->cstate = C_DATA_END; c->llen = 0; c->long_line = false; }
            continue;
        }
        char ch = (char)*p++;
        n--;
        if (ch == '\n') {
            if (c->llen && c->line[c->llen - 1] == '\r') c->llen--;
            c->line[c->llen] = 0;
            bool too_long = c->long_line;
            c->llen = 0;
            c->long_line = false;
            if (too_long && c->cstate != C_TRAILER) { fail(c, "bad chunked body"); return; }
            chunk_line(c);
        } else if (c->llen < 64) {                   /* a size line, or the start of a trailer */
            c->line[c->llen++] = ch;
        } else {
            c->long_line = true;
        }
    }
}

/* Bytes of the 200 answer's body. */
static void body_bytes(httpc_t *c, const uint8_t *p, size_t n)
{
    if (c->mode == B_CHUNKED) { chunked_bytes(c, p, n); return; }
    if (c->mode == B_LENGTH) {
        if (n > (size_t)c->left) n = (size_t)c->left;    /* anything after it isn't the body */
        c->left -= (long)n;
    }
    deliver(c, p, n);
    if (c->state == H_BODY && c->mode == B_LENGTH && !c->left) finish(c);
}

/* ------------------------------------------------------------ head */

/* Follow a redirect to c->path (where the Location went). */
static void redirect(httpc_t *c)
{
    if (c->loc_too_long) { fail(c, "redirect address too long"); return; }
    if (++c->redirects > HTTPC_REDIRECTS_MAX) { fail(c, "too many redirects"); return; }
    bool was_https = c->https;
    const char *e = NULL;
    if (c->path[0] == '/' && c->path[1] != '/') {    /* on the same server */
        c->path[strcspn(c->path, "#")] = 0;
        if (!url_chars_ok(c->path)) e = "bad redirect address";
    } else {
        e = set_target(c, c->path);
    }
    if (!e && was_https && !c->https) e = "redirect to plain http refused";
    conn_close(&c->conn);
    if (!e) e = start(c);
    if (e) fail(c, "%s", e);
}

/* The head is complete: follow it, fail, or go on to the body. */
static void head_done(httpc_t *c)
{
    int st = c->status;
    if (st >= 100 && st < 200) {                     /* an interim answer: the real one follows */
        c->status = 0;
        return;
    }
    if (st == 301 || st == 302 || st == 303 || st == 307 || st == 308) {
        if (!c->have_loc) fail(c, "HTTP %d without a Location", st);
        else redirect(c);
        return;
    }
    if (st != 200) { fail(c, "HTTP %d", st); return; }
    c->state = H_BODY;
    if (c->chunked) {
        c->mode = B_CHUNKED;
        c->cstate = C_SIZE;
        c->length = -1;
        c->llen = 0;
        c->long_line = false;
    } else if (c->length >= 0) {
        c->mode = B_LENGTH;
        c->left = c->length;
        if (!c->left) finish(c);
    } else {
        c->mode = B_CLOSE;
    }
}

static bool header_is(const char *line, const char *name, const char **value)
{
    size_t n = strlen(name);
    if (strncasecmp(line, name, n) || line[n] != ':') return false;
    const char *v = line + n + 1;
    while (*v == ' ' || *v == '\t') v++;
    *value = v;
    return true;
}

/* A line of the head is complete (in c->line, without its CRLF). */
static void head_line(httpc_t *c)
{
    char *s = c->line;
    if (!c->status) {
        char *sp = strchr(s, ' '), *end = NULL;
        long st = sp ? strtol(sp + 1, &end, 10) : 0;
        if (strncmp(s, "HTTP/1.", 7) || !sp || end != sp + 4 || st < 100 || st > 999) {
            fail(c, "not an HTTP answer");
            return;
        }
        c->status = (int)st;
        return;
    }
    if (!*s) { head_done(c); return; }
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t')) s[--n] = 0;
    const char *v;
    if (header_is(s, "Location", &v)) {
        c->have_loc = true;
        c->loc_too_long = c->long_line || strlen(v) >= URL_MAX;
        if (!c->loc_too_long) memmove(c->path, v, strlen(v) + 1);
    } else if (header_is(s, "Content-Length", &v)) {
        char *end;
        long len = strtol(v, &end, 10);
        if (end == v || *end || len < 0) { fail(c, "bad Content-Length"); return; }
        c->length = len;
    } else if (header_is(s, "Transfer-Encoding", &v)) {
        for (const char *t = v; *t; t++) if (!strncasecmp(t, "chunked", 7)) c->chunked = true;
    }
}

/* Bytes of the head; once it's complete, the rest are the body's. */
static void head_bytes(httpc_t *c, const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (c->state != H_HEAD) {
            if (c->state == H_BODY) body_bytes(c, p + i, n - i);
            return;
        }
        if (++c->head_total > HEAD_TOTAL_MAX) { fail(c, "answer head too long"); return; }
        char ch = (char)p[i];
        if (ch == '\n') {
            if (c->llen && c->line[c->llen - 1] == '\r') c->llen--;
            c->line[c->llen] = 0;
            head_line(c);
            c->llen = 0;
            c->long_line = false;
        } else if (c->llen < sizeof c->line - 1) {
            c->line[c->llen++] = ch;
        } else {
            c->long_line = true;                     /* (a long policy header: its end isn't needed) */
        }
    }
}

/* ------------------------------------------------------------ steps */

static void step(httpc_t *c)
{
    if (c->state == H_CONNECT) {
        conn_state_t was = c->conn.state;
        conn_step(&c->conn);
        if (c->conn.state != was) progress(c);
        if (c->conn.state == CONN_CLOSED) { conn_failed(c, NULL); return; }
        if (c->conn.state != CONN_OPEN) return;
        c->state = H_SEND;
    }
    if (c->state == H_SEND) {
        while (c->out_off < c->out_len) {
            int n = conn_send(&c->conn, c->line + c->out_off, c->out_len - c->out_off);
            if (n < 0) { conn_failed(c, "send"); return; }
            if (n == 0) return;
            c->out_off += (size_t)n;
            progress(c);
        }
        c->state = H_HEAD;
        c->llen = 0;
    }
    /* The head and the body: what has come, up to POLL_BYTES. A redirect
       starts over (a new connection, next time). */
    for (size_t got = 0; got < POLL_BYTES && (c->state == H_HEAD || c->state == H_BODY);) {
        int n = conn_recv(&c->conn, c->rx, sizeof c->rx);
        if (n == 0) return;
        if (n == CONN_FAIL) { conn_failed(c, "receive"); return; }
        if (n == CONN_EOF) {
            if (c->state == H_BODY && c->mode == B_CLOSE) finish(c);
            else fail(c, "connection closed early");
            return;
        }
        progress(c);
        got += (size_t)n;
        if (c->state == H_HEAD) head_bytes(c, c->rx, (size_t)n);
        else body_bytes(c, c->rx, (size_t)n);
    }
}

httpc_status_t httpc_poll(httpc_t *c)
{
    if (c->state != H_DONE && c->state != H_FAILED) {
        step(c);
        if (c->state != H_DONE && c->state != H_FAILED && hw_millis() - c->last > HTTPC_IDLE_MS) fail(c, "timed out");
    }
    return c->state == H_DONE ? HTTPC_DONE : c->state == H_FAILED ? HTTPC_FAILED : HTTPC_BUSY;
}

long httpc_length(const httpc_t *c) { return c->status == 200 && c->state >= H_BODY ? c->length : -1; }

const char *httpc_error(const httpc_t *c) { return c->err; }

bool httpc_tethered(const httpc_t *c) { return c->conn.tun >= 0; }

void httpc_free(httpc_t *c)
{
    if (!c) return;
    conn_close(&c->conn);
    free(c);
}

const char *httpc_short_error(const char *err, char *buf, size_t cap)
{
    if (!strncmp(err, "HTTP ", 5)) {
        snprintf(buf, cap, "server error %d", atoi(err + 5));
        return buf;
    }
    if (!strncmp(err, "TLS", 3)) return "TLS error";
    if (!strcmp(err, "connection closed early") || !strncmp(err, "receive:", 8)) return "download cut short";
    if (!strncmp(err, "cannot ", 7) || !strncmp(err, "socket:", 7) || !strncmp(err, "send:", 5))
        return "no connection";
    /* "timed out", "too many redirects", "out of memory", ...: already short. */
    return strlen(err) <= HTTPC_SHORT_MAX ? err : "download failed";
}
