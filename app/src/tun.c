/* The USB tether's connections and frames. See tun.h. */
#include "tun.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "b64.h"

typedef struct {
    bool used;
    uint32_t id;               /* on the wire */
    tun_state_t state;
    char why[96];              /* TUN_CLOSED: "" ended normally, else what went wrong */
    uint8_t *rx;               /* TUN_WINDOW bytes from the computer, a ring */
    size_t rx_head, rx_len;
    size_t rx_taken;           /* read from rx since the last ACK we sent */
    size_t tx_unacked;         /* sent, not acknowledged by the computer yet */
} chan_t;

static tun_io_t s_io;
static chan_t s_ch[TUN_MAX];
static bool s_up;
static uint32_t s_heard;        /* when the computer was last heard */
static uint32_t s_next_id = 1;  /* never used twice while the device runs */

/* Why a connection ends when one of its frames had to be dropped: the
   computer would wait for it forever. */
#define LOST "USB tether: the computer stopped reading"

/* "TUN DATA <id> <base64>\n", the longest line made here. */
static char s_line[sizeof "TUN DATA 4294967295 \n" + (TUN_DATA_MAX + 2) / 3 * 4];

void tun_init(const tun_io_t *io)
{
    s_io = *io;
    s_next_id = io->first_id ? io->first_id : 1;
}

/* A frame. False if it couldn't go out. */
static bool sendf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static bool sendf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(s_line, sizeof s_line, fmt, ap);
    va_end(ap);
    return n > 0 && (size_t)n < sizeof s_line && s_io.out(s_line, (size_t)n);
}

static void set_closed(chan_t *c, const char *why)
{
    c->state = TUN_CLOSED;
    snprintf(c->why, sizeof c->why, "%s", why);
}

/* A frame we can't take: the connection ends, at both ends. */
static void fail(chan_t *c, const char *what)
{
    sendf("TUN CLOSE %lu %s\n", (unsigned long)c->id, what);
    char why[sizeof c->why];
    snprintf(why, sizeof why, "USB tether: %s", what);
    set_closed(c, why);
}

/* ------------------------------------------------------------- tether */

bool tun_on(uint32_t now_ms)
{
    bool starts = !s_up;
    s_up = true;
    s_heard = now_ms;
    return starts;
}

void tun_heard(uint32_t now_ms)
{
    if (s_up) s_heard = now_ms;
}

void tun_off(void)
{
    s_up = false;
    for (int i = 0; i < TUN_MAX; i++)
        if (s_ch[i].used && s_ch[i].state != TUN_CLOSED) set_closed(&s_ch[i], "the computer's network went away");
}

void tun_poll(uint32_t now_ms)
{
    if (s_up && now_ms - s_heard > TUN_KEEP_MS) tun_off();
}

bool tun_up(void) { return s_up; }

/* ------------------------------------------------------------- frames */

/* The next space-separated word of *p: its start, *len its length. */
static const char *word(const char **p, size_t *len)
{
    const char *s = *p;
    while (*s == ' ') s++;
    *len = strcspn(s, " ");
    *p = s + *len;
    return s;
}

static bool number(const char *s, size_t len, uint32_t *out)
{
    if (len == 0 || len > 10) return false;
    uint64_t v = 0;
    for (size_t i = 0; i < len; i++) {
        if (s[i] < '0' || s[i] > '9') return false;
        v = v * 10 + (uint64_t)(s[i] - '0');
    }
    if (v > UINT32_MAX) return false;
    *out = (uint32_t)v;
    return true;
}

static bool is(const char *w, size_t len, const char *name)
{
    if (len != strlen(name)) return false;
    for (size_t i = 0; i < len; i++) {
        char c = w[i];
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        if (c != name[i]) return false;
    }
    return true;
}

static void got_data(chan_t *c, const char *text, size_t len)
{
    static uint8_t buf[TUN_DATA_MAX];
    if (c->state != TUN_OPEN) { fail(c, "bad frame"); return; }
    long n = b64_decode(text, len, buf, sizeof buf);
    if (n <= 0) { fail(c, "bad frame"); return; }
    if (c->rx_len + (size_t)n > TUN_WINDOW) { fail(c, "window exceeded"); return; }
    for (long i = 0; i < n; i++) c->rx[(c->rx_head + c->rx_len++) % TUN_WINDOW] = buf[i];
}

void tun_frame(const char *frame)
{
    const char *p = frame;
    size_t klen, ilen, alen;
    const char *kind = word(&p, &klen), *ids = word(&p, &ilen);
    uint32_t id;
    if (!number(ids, ilen, &id)) return;
    chan_t *c = NULL;
    for (int i = 0; i < TUN_MAX && !c; i++)
        if (s_ch[i].used && s_ch[i].id == id && s_ch[i].state != TUN_CLOSED) c = &s_ch[i];
    if (!c) {                                    /* closed already, or never ours: */
        if (!is(kind, klen, "CLOSE")) sendf("TUN CLOSE %lu\n", (unsigned long)id);   /* the computer drops it too */
        return;
    }
    const char *rest = p + strspn(p, " ");
    const char *arg = word(&p, &alen);
    if (is(kind, klen, "OPENED")) {
        if (c->state == TUN_OPENING) c->state = TUN_OPEN;
        else fail(c, "bad frame");
    } else if (is(kind, klen, "DATA")) {
        got_data(c, arg, alen);
    } else if (is(kind, klen, "ACK")) {
        uint32_t n;
        if (!number(arg, alen, &n) || n > c->tx_unacked) fail(c, "bad frame");
        else c->tx_unacked -= n;
    } else if (is(kind, klen, "CLOSE")) {
        set_closed(c, rest);                     /* why: the rest of the line */
    } else {
        fail(c, "bad frame");
    }
}

/* -------------------------------------------------------- connections */

static chan_t *chan(int h)
{
    return h >= 0 && h < TUN_MAX && s_ch[h].used ? &s_ch[h] : NULL;
}

int tun_open(const char *host, uint16_t port, const char **why)
{
    size_t len = strlen(host);
    bool ok = len > 0 && len < TUN_HOST_MAX && port > 0;
    for (size_t i = 0; ok && i < len; i++) ok = host[i] > ' ' && host[i] < 127;
    if (!ok) { *why = "bad host name"; return -1; }
    if (!s_up) { *why = "no USB tether"; return -1; }
    int h = 0;
    while (h < TUN_MAX && s_ch[h].used) h++;
    if (h == TUN_MAX) { *why = "too many connections through the USB tether"; return -1; }
    chan_t *c = &s_ch[h];
    memset(c, 0, sizeof *c);
    c->rx = s_io.alloc(TUN_WINDOW);
    if (!c->rx) { *why = "out of memory"; return -1; }
    c->used = true;
    c->id = s_next_id++;
    if (!s_next_id) s_next_id = 1;
    c->state = TUN_OPENING;
    if (!sendf("TUN OPEN %lu %s %u\n", (unsigned long)c->id, host, (unsigned)port)) set_closed(c, LOST);
    return h;
}

tun_state_t tun_state(int h, const char **why)
{
    chan_t *c = chan(h);
    if (!c) { *why = "not open"; return TUN_CLOSED; }
    *why = c->why;
    return c->state;
}

int tun_send(int h, const void *data, size_t len)
{
    chan_t *c = chan(h);
    if (c && c->state == TUN_CLOSED && !c->why[0] && c->rx_len) return (int)len;   /* (see tun.h) */
    if (!c || c->state == TUN_CLOSED) return TUN_FAIL;
    if (c->state == TUN_OPENING) return 0;
    const uint8_t *p = data;
    size_t sent = 0;
    while (sent < len && c->tx_unacked < TUN_WINDOW) {
        size_t n = TUN_WINDOW - c->tx_unacked;
        if (n > TUN_DATA_MAX) n = TUN_DATA_MAX;
        if (n > len - sent) n = len - sent;
        int head = snprintf(s_line, sizeof s_line, "TUN DATA %lu ", (unsigned long)c->id);
        size_t at = (size_t)head + b64_encode(p + sent, n, s_line + head, false);
        s_line[at++] = '\n';
        if (!s_io.out(s_line, at)) { set_closed(c, LOST); return TUN_FAIL; }
        c->tx_unacked += n;
        sent += n;
    }
    return (int)sent;
}

int tun_recv(int h, void *buf, size_t cap)
{
    chan_t *c = chan(h);
    if (!c) return TUN_FAIL;
    if (c->rx_len == 0) {
        if (c->state != TUN_CLOSED) return 0;
        return c->why[0] ? TUN_FAIL : TUN_EOF;
    }
    size_t n = c->rx_len < cap ? c->rx_len : cap;
    uint8_t *o = buf;
    for (size_t i = 0; i < n; i++) o[i] = c->rx[(c->rx_head + i) % TUN_WINDOW];
    c->rx_head = (c->rx_head + n) % TUN_WINDOW;
    c->rx_len -= n;
    c->rx_taken += n;
    /* The computer may send more once it hears: tell it when the buffer is
       empty or a good part of the window is free (not after every read). */
    if (c->state != TUN_CLOSED && (c->rx_len == 0 || c->rx_taken >= TUN_WINDOW / 4)) {
        if (!sendf("TUN ACK %lu %lu\n", (unsigned long)c->id, (unsigned long)c->rx_taken)) set_closed(c, LOST);
        c->rx_taken = 0;
    }
    return (int)n;
}

void tun_close(int h)
{
    chan_t *c = chan(h);
    if (!c) return;
    if (c->state != TUN_CLOSED && s_up) sendf("TUN CLOSE %lu\n", (unsigned long)c->id);
    s_io.free(c->rx);
    memset(c, 0, sizeof *c);
}
