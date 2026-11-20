#include <stdlib.h>
#include <string.h>

#include "b64.h"
#include "check.h"
#include "tun.h"

void test_tun(int *total, int *failed);

/* What tun.c sent, the lines one after another. */
static char s_out[8192];
static size_t s_out_len;
static int s_allocs;
static bool s_drop;             /* the computer stopped reading: lines are dropped */

static bool out(const char *line, size_t len)
{
    if (s_drop) return false;
    if (s_out_len + len < sizeof s_out) memcpy(s_out + s_out_len, line, len);
    s_out_len += len;
    s_out[s_out_len < sizeof s_out ? s_out_len : sizeof s_out - 1] = 0;
    return true;
}

static void *alloc(size_t n) { s_allocs++; return malloc(n); }
static void release(void *p) { if (p) s_allocs--; free(p); }

/* The lines sent since the last call, and forget them. */
static const char *sent(void)
{
    static char got[sizeof s_out];
    memcpy(got, s_out, sizeof got);
    s_out_len = 0;
    s_out[0] = 0;
    return got;
}

/* The USB tether's frames and windows (docs/PROTOCOL.md, "USB tether"). */
void test_tun(int *total, int *failed)
{
    tun_init(&(tun_io_t){ .out = out, .alloc = alloc, .free = release });
    const char *why;
    char buf[TUN_WINDOW + 16];

    /* only while the computer is heard from: TUN ON, or any other line */
    CHECK(tun_open("example.com", 443, &why) == -1 && !strcmp(why, "no USB tether"));
    CHECK(tun_on(1000) && !tun_on(3000) && tun_up());
    tun_poll(3000 + TUN_KEEP_MS);
    CHECK(tun_up());
    tun_heard(4000);
    tun_poll(3001 + TUN_KEEP_MS);
    CHECK(tun_up());
    tun_poll(4001 + TUN_KEEP_MS);
    CHECK(!tun_up());
    tun_heard(9000);                             /* (doesn't start it) */
    CHECK(!tun_up());
    CHECK(tun_on(10000));

    /* a host can't break the line */
    CHECK(tun_open("a b", 80, &why) == -1 && !strcmp(why, "bad host name"));
    CHECK(tun_open("a\nTUN OFF", 80, &why) == -1);
    CHECK(tun_open("", 80, &why) == -1);
    CHECK(tun_open("example.com", 0, &why) == -1);
    CHECK(!strcmp(sent(), ""));

    /* open: nothing goes until the computer has connected */
    int h = tun_open("example.com", 443, &why);
    CHECK(h >= 0 && !strcmp(sent(), "TUN OPEN 1 example.com 443\n"));
    CHECK(tun_state(h, &why) == TUN_OPENING);
    CHECK(tun_send(h, "x", 1) == 0 && tun_recv(h, buf, 10) == 0);
    tun_frame("OPENED 2");                       /* not ours: the computer is to drop it */
    CHECK(tun_state(h, &why) == TUN_OPENING && !strcmp(sent(), "TUN CLOSE 2\n"));
    tun_frame("CLOSE 2");                        /* (CLOSE gets no answer) */
    CHECK(!strcmp(sent(), ""));
    tun_frame("opened 1");                       /* (either case) */
    CHECK(tun_state(h, &why) == TUN_OPEN);

    /* sending: at most TUN_DATA_MAX a frame, TUN_WINDOW unacknowledged */
    static uint8_t big[TUN_WINDOW + 100];
    for (size_t i = 0; i < sizeof big; i++) big[i] = (uint8_t)(i * 7);
    CHECK(tun_send(h, big, 2000) == 2000);
    const char *lines = sent(), *second = strchr(lines, '\n') + 1;
    CHECK(!strncmp(lines, "TUN DATA 1 ", 11) && !strncmp(second, "TUN DATA 1 ", 11));
    CHECK(b64_decode(lines + 11, (size_t)(second - lines - 12), buf, sizeof buf) == TUN_DATA_MAX &&
          !memcmp(buf, big, TUN_DATA_MAX));
    CHECK(b64_decode(second + 11, strlen(second) - 12, buf, sizeof buf) == 500 && !memcmp(buf, big + TUN_DATA_MAX, 500));
    CHECK(tun_send(h, big, sizeof big) == TUN_WINDOW - 2000);  /* the rest of the window */
    CHECK(tun_send(h, big, sizeof big) == 0);
    sent();
    tun_frame("ACK 1 100");
    CHECK(tun_send(h, big, sizeof big) == 100);
    sent();

    /* receiving: read, then acknowledged */
    tun_frame("DATA 1 aGVsbG8=");
    CHECK(!strcmp(sent(), ""));
    CHECK(tun_recv(h, buf, 3) == 3 && !memcmp(buf, "hel", 3));
    CHECK(!strcmp(sent(), ""));                  /* not after every read */
    CHECK(tun_recv(h, buf, 10) == 2 && !memcmp(buf, "lo", 2));
    CHECK(!strcmp(sent(), "TUN ACK 1 5\n"));     /* the buffer is empty */
    CHECK(tun_recv(h, buf, 10) == 0);

    /* a quarter of the window read is acknowledged, and the ring wraps */
    char frame[64 + (TUN_DATA_MAX + 2) / 3 * 4];
    enum { PIECE = 750, PIECES = 6 };            /* 4500 bytes a round */
    for (int round = 0; round < 2; round++) {
        for (int i = 0; i < PIECES; i++) {
            strcpy(frame, "DATA 1 ");
            b64_encode(big + i * PIECE, PIECE, frame + 7, false);
            tun_frame(frame);
        }
        if (round == 0) {
            CHECK(tun_recv(h, buf, TUN_WINDOW / 4) == TUN_WINDOW / 4 && !memcmp(buf, big, TUN_WINDOW / 4));
            CHECK(!strcmp(sent(), "TUN ACK 1 2048\n"));
            CHECK(tun_recv(h, buf, sizeof buf) == PIECES * PIECE - TUN_WINDOW / 4);
            CHECK(!memcmp(buf, big + TUN_WINDOW / 4, PIECES * PIECE - TUN_WINDOW / 4));
            CHECK(!strcmp(sent(), "TUN ACK 1 2452\n"));
        } else {                                 /* (from 4505 on: past the end) */
            CHECK(tun_recv(h, buf, sizeof buf) == PIECES * PIECE && !memcmp(buf, big, PIECES * PIECE));
            CHECK(!strcmp(sent(), "TUN ACK 1 4500\n"));
        }
    }

    /* the computer ends it: what came before is read first, and while
       there's some, what's sent goes nowhere (as on a socket) */
    tun_frame("DATA 1 aGk=");
    tun_frame("CLOSE 1");
    CHECK(tun_state(h, &why) == TUN_CLOSED && !strcmp(why, ""));
    CHECK(tun_send(h, "x", 1) == 1 && !strcmp(sent(), ""));
    CHECK(tun_recv(h, buf, 10) == 2 && tun_recv(h, buf, 10) == TUN_EOF);
    CHECK(tun_send(h, "x", 1) == TUN_FAIL);
    tun_close(h);
    CHECK(!strcmp(sent(), ""));                  /* it knows; and no ACK after its CLOSE */

    /* ids are never used again, and a failure says why */
    h = tun_open("example.com", 80, &why);
    CHECK(!strcmp(sent(), "TUN OPEN 2 example.com 80\n"));
    tun_frame("DATA 1 aGk=");                    /* the old one's: not taken for this one */
    CHECK(!strcmp(sent(), "TUN CLOSE 1\n"));
    tun_frame("CLOSE 2 connection refused");
    CHECK(tun_state(h, &why) == TUN_CLOSED && !strcmp(why, "connection refused"));
    CHECK(tun_recv(h, buf, 10) == TUN_FAIL);
    tun_close(h);

    /* frames the device can't take end the connection, at both ends */
    h = tun_open("x", 1, &why);
    sent();
    tun_frame("DATA 3 aGk=");                    /* before OPENED */
    CHECK(!strcmp(sent(), "TUN CLOSE 3 bad frame\n"));
    CHECK(tun_state(h, &why) == TUN_CLOSED && !strcmp(why, "USB tether: bad frame"));
    tun_close(h);
    /* (after OPENED: bad base64, no data, an ACK of more than was sent, a
       second OPENED, an unknown frame) */
    const struct { const char *kind, *arg; } bad[] = {
        { "DATA", "*" }, { "DATA", "" }, { "ACK", "1" }, { "ACK", "x" }, { "OPENED", "" }, { "WHAT", "" },
    };
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
        unsigned id = 4 + (unsigned)i;
        h = tun_open("x", 1, &why);
        char want[64], f[40];
        snprintf(want, sizeof want, "TUN OPEN %u x 1\nTUN CLOSE %u bad frame\n", id, id);
        snprintf(f, sizeof f, "OPENED %u", id);
        tun_frame(f);
        snprintf(f, sizeof f, "%s %u %s", bad[i].kind, id, bad[i].arg);
        tun_frame(f);
        CHECK(!strcmp(sent(), want));
        tun_close(h);
    }
    h = tun_open("x", 1, &why);                  /* id 10 */
    tun_frame("OPENED 10");
    sent();
    for (int i = 0; i <= TUN_WINDOW / TUN_DATA_MAX; i++) {
        strcpy(frame, "DATA 10 ");
        b64_encode(big, TUN_DATA_MAX, frame + 8, false);
        tun_frame(frame);
    }
    CHECK(!strcmp(sent(), "TUN CLOSE 10 window exceeded\n"));
    tun_close(h);
    tun_frame("rubbish");                        /* no id: nothing to answer */
    tun_frame("");
    CHECK(!strcmp(sent(), ""));

    /* closing an open one tells the computer */
    h = tun_open("x", 1, &why);
    sent();
    tun_close(h);
    CHECK(!strcmp(sent(), "TUN CLOSE 11\n"));

    /* the tether ends: every connection with it */
    int a = tun_open("x", 1, &why), b = tun_open("y", 2, &why);
    tun_frame("OPENED 12");
    sent();
    tun_off();
    CHECK(tun_state(a, &why) == TUN_CLOSED && !strcmp(why, "the computer's network went away"));
    CHECK(tun_state(b, &why) == TUN_CLOSED);
    CHECK(tun_recv(a, buf, 10) == TUN_FAIL);
    tun_close(a);
    tun_close(b);
    CHECK(!strcmp(sent(), ""));                  /* nobody to tell */
    CHECK(s_allocs == 0);

    /* a frame the computer doesn't read (it stopped) ends its connection:
       the computer would wait for it forever */
    tun_on(20000);
    h = tun_open("x", 1, &why);                  /* id 14 */
    tun_frame("OPENED 14");
    tun_frame("DATA 14 aGk=");
    s_drop = true;
    CHECK(tun_recv(h, buf, 10) == 2);            /* (its ACK is lost) */
    CHECK(tun_state(h, &why) == TUN_CLOSED && !strcmp(why, "USB tether: the computer stopped reading"));
    tun_close(h);
    s_drop = false;
    h = tun_open("x", 1, &why);
    tun_frame("OPENED 15");
    s_drop = true;
    CHECK(tun_send(h, "x", 1) == TUN_FAIL && tun_state(h, &why) == TUN_CLOSED);
    tun_close(h);
    h = tun_open("x", 1, &why);
    CHECK(tun_state(h, &why) == TUN_CLOSED && !strcmp(why, "USB tether: the computer stopped reading"));
    tun_close(h);
    s_drop = false;
    tun_off();
    sent();

    /* as many connections as TUN_MAX */
    tun_on(20000);
    int hs[TUN_MAX];
    for (int i = 0; i < TUN_MAX; i++) CHECK((hs[i] = tun_open("x", 1, &why)) >= 0);
    CHECK(tun_open("x", 1, &why) == -1 && !strcmp(why, "too many connections through the USB tether"));
    for (int i = 0; i < TUN_MAX; i++) tun_close(hs[i]);
    tun_off();
    sent();
    CHECK(s_allocs == 0);

    /* ids start where the firmware says (a random number at boot) */
    tun_init(&(tun_io_t){ .out = out, .alloc = alloc, .free = release, .first_id = 4000000000u });
    tun_on(30000);
    h = tun_open("x", 1, &why);
    CHECK(!strcmp(sent(), "TUN OPEN 4000000000 x 1\n"));
    tun_close(h);
    tun_off();
    sent();

    *total += check_total;
    *failed += check_failed;
}
