/*
 * The USB tether: TCP connections that the computer on the USB link makes
 * for the device. Spec: docs/PROTOCOL.md, "USB tether". This keeps the
 * connections, their buffers and windows, and reads and writes the frames;
 * protocol.c hands it the computer's TUN lines and sends out the ones it
 * makes. It neither reads the clock nor does I/O itself (fw-unit tests it).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TUN_MAX      20      /* connections at a time: the scripts' 16 sockets and the firmware's HTTP */
#define TUN_WINDOW   8192    /* bytes of DATA one side may have unacknowledged */
#define TUN_DATA_MAX 1500    /* bytes in one DATA frame (its line stays under PROTO_LINE_MAX) */
#define TUN_KEEP_MS  5000    /* the tether ends this long after the computer was last heard */
#define TUN_HOST_MAX 128     /* a host name, with its NUL */

#define TUN_EOF  (-1)        /* tun_recv: the connection ended normally */
#define TUN_FAIL (-2)        /* tun_recv, tun_send: it failed (why: tun_state) */

typedef struct {
    /** Send a frame: a whole line, with its "\n". False if it was dropped
     *  (the computer stopped reading). */
    bool (*out)(const char *line, size_t len);
    /** Room for a connection's TUN_WINDOW bytes, and its return. */
    void *(*alloc)(size_t n);
    void (*free)(void *p);
    /** The first connection's id (0: 1). Random, so that a restarted
     *  device's ids don't meet the frames of its earlier run's. */
    uint32_t first_id;
} tun_io_t;

typedef enum { TUN_OPENING, TUN_OPEN, TUN_CLOSED } tun_state_t;

/** Once, before anything else (io is copied). */
void tun_init(const tun_io_t *io);

/** TUN ON came: the tether is on until TUN_KEEP_MS after the computer was
 *  last heard. True if it starts now. */
bool tun_on(uint32_t now_ms);
/** A line from the computer (any, once it's handled): the tether stays on. */
void tun_heard(uint32_t now_ms);
/** End the tether (TUN OFF, or no TUN ON in time): every connection
 *  through it closes, why "the computer's network went away". */
void tun_off(void);
/** From the main loop: ends the tether when the computer has gone quiet. */
void tun_poll(uint32_t now_ms);
bool tun_up(void);

/** One of the computer's frames, the words after "TUN": "OPENED <id>",
 *  "DATA <id> <base64>", "ACK <id> <n>" or "CLOSE <id> [<why>]". One for a
 *  connection that isn't open is answered with CLOSE. */
void tun_frame(const char *frame);

/** Ask the computer to connect. A handle (0..TUN_MAX-1), or -1 and why. */
int tun_open(const char *host, uint16_t port, const char **why);
/** TUN_CLOSED: *why is "" if it ended normally, else what went wrong. */
tun_state_t tun_state(int h, const char **why);
/** Bytes taken, as many as the window has room for (in frames of up to
 *  TUN_DATA_MAX; 0 while opening or with the window full), or TUN_FAIL once
 *  closed. After the computer has ended it normally, bytes still to be read
 *  are kept: until they're read, what's sent is taken and dropped, as a
 *  socket would. */
int tun_send(int h, const void *data, size_t len);
/** Bytes read into buf, 0 if none yet, TUN_EOF or TUN_FAIL at the end (the
 *  bytes that came before a CLOSE are read first). */
int tun_recv(int h, void *buf, size_t cap);
/** Done with it: tells the computer if it's still open, frees the handle. */
void tun_close(int h);
