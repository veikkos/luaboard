/*
 * The host: what the simulator needs from the computer it runs on, beyond
 * standard C and the POSIX subset the firmware itself uses (which Windows
 * gets from platform/host/win32/posix).
 *
 * Implementations: platform/host/posix (macOS, Linux) and platform/host/win32.
 *
 *   - the serial link: the board's USB port as the PC sees it. A pty on
 *     POSIX with a symlink, a named pipe on Windows; by default the first
 *     free of esp32-sim, esp32-sim-2 ... esp32-sim-16 (/tmp/esp32-sim...,
 *     \\.\pipe\esp32-sim...), where the tools look
 *   - the control channel (--control): lines in, answers out on the same
 *     connection, a TCP port on 127.0.0.1 everywhere
 *   - handing both over to the next boot, which may be another executable:
 *     the simulator replaces itself at every reset (host_become)
 *   - crashes of the firmware, mDNS, the executable's own path and identity
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef _WIN32
#define HOST_EXE_SUFFIX ".exe"
#else
#define HOST_EXE_SUFFIX ""
#endif

#if defined(_MSC_VER)
#define HOST_NORETURN __declspec(noreturn)
#else
#define HOST_NORETURN __attribute__((noreturn))
#endif

/** Once, first thing in main(): the process settings the simulator wants
 *  (no SIGPIPE on POSIX; sockets, timer resolution, binary files and no
 *  error dialogs on Windows). */
void host_init(void);

/* ---- clocks ------------------------------------------------------------ */
/** A monotonic clock, µs. */
int64_t host_mono_us(void);
/** This computer's wall clock, µs since 1970 (UTC). */
int64_t host_utc_us(void);
/** The CPU time the calling thread has used, µs: it doesn't grow while the
 *  thread waits, or while the computer runs other things instead. */
int64_t host_thread_cpu_us(void);
void host_sleep_ms(int ms);

/* ---- files --------------------------------------------------------------- */
/** The running executable, an absolute path. */
void host_self_path(char *out, size_t cap);
/** At power-on: let a new build replace the executable started by hand
 *  while it runs (--watch, the build's rebuild-on-save). Windows locks a
 *  running program's file, so it runs on from the file moved aside, and a
 *  copy takes its place; nothing to do on POSIX. */
void host_unlock_self(void);
/** Which file is at `path` (volume, file index or inode, size, time): a new
 *  build gets a new id. "" if there is none. */
void host_file_id(const char *path, char *out, size_t cap);
/** Make `path` runnable (the executable bit; nothing to do on Windows). */
bool host_set_executable(const char *path, bool on);
/** Can `path` be run (exists, executable bit)? */
bool host_is_executable(const char *path);
/** Put `from` in place of `to`, even when `to` is a program that is still
 *  running (Windows: it is moved aside first, and cleaned up later). */
bool host_replace_file(const char *from, const char *to);
/** Is `head` (the first bytes of a file) a program this kind of host runs:
 *  Mach-O or ELF on POSIX, PE (MZ) on Windows? */
bool host_is_program(const unsigned char *head, size_t len);

/* ---- the serial link --------------------------------------------------- */
typedef struct host_link host_link_t;

/** Create the link, at power-on. `name`: where the PC finds it (POSIX: the
 *  path of a symlink to the pty; Windows: the pipe name), never one another
 *  simulator has; NULL for the first free of the family (host_text.h,
 *  host_link_family). `where` gets what to tell the user. */
host_link_t *host_link_create(const char *name, char *where, size_t cap);
/** Take over the link from the boot before, from its handover text (see
 *  host_handover); NULL if there is none in it. */
host_link_t *host_link_adopt(const char *handover);
/** The port as the PC opens it (pty path, \\.\pipe\name). */
const char *host_link_port(const host_link_t *l);
/** Where the PC finds the link, as the user names it (POSIX: the symlink,
 *  Windows: the pipe); NULL if it has no name (POSIX without a symlink). */
const char *host_link_name(const host_link_t *l);
/** Up to `len` bytes that arrived; waits up to `timeout_ms` for the first
 *  (-1: for ever). 0 if none came. HOST_LINK_HUNG_UP if it returned at once
 *  because nobody has the other end open: the caller waits out the time
 *  itself (on the simulated clock), as on an idle line. */
#define HOST_LINK_HUNG_UP (-2)
int host_link_read(host_link_t *l, void *buf, size_t len, int timeout_ms);
/** Write, waiting at most `stall_ms` (-1: for ever) for the PC to take
 *  bytes when its buffer is full; returns how many went (the rest is
 *  dropped, like a cable nobody listens on). */
int host_link_write(host_link_t *l, const void *buf, size_t len, int stall_ms);
/** Wait up to `ms` for input on the link or on one of the `n` sockets
 *  `socks` (not on Windows: the link only); true if there is some. Ends
 *  early (false) on host_link_kick. */
bool host_link_wait(host_link_t *l, int ms, const int *socks, int n);
/** End a host_link_wait now, from any thread. */
void host_link_kick(host_link_t *l);
/** At the end of the simulator: remove what the user would find (the symlink
 *  host_link_create made, if it still points to this link). */
void host_link_remove(host_link_t *l);

/* ---- the control channel ----------------------------------------------- */
typedef struct host_ctl host_ctl_t;

/** Create it at power-on, listening at `path`: [127.0.0.1:]PORT, 0 for any
 *  free port (printed as "Control: 127.0.0.1:N"). NULL on error (said on
 *  stderr). */
host_ctl_t *host_ctl_create(const char *path);
host_ctl_t *host_ctl_adopt(const char *handover);
/** Bytes of lines that arrived, without waiting; 0 if none. */
int host_ctl_read(host_ctl_t *c, void *buf, size_t len);
/** Send an answer line (with its newline). Lost if nobody listens. */
void host_ctl_answer(host_ctl_t *c, const char *text, size_t len);
/** Wait up to `ms` for a line to arrive. */
void host_ctl_wait(host_ctl_t *c, int ms);
/** At the end of the simulator: clean up what it left (nothing so far). */
void host_ctl_remove(host_ctl_t *c, const char *path);
/** The TCP port it listens on (0 if unknown). */
uint16_t host_ctl_port(const host_ctl_t *c);

/* ---- one boot hands over to the next ---------------------------------- */
/** The handover text for the link and the control channel (either may be
 *  NULL): comma-separated key=value, put in front of the simulator's own. */
void host_handover(const host_link_t *l, const host_ctl_t *c, char *out, size_t cap);
/** Replace this process with `image` run with `argv` (argv[0] the image):
 *  the link and the control channel go along, nothing else (Wi-Fi sockets,
 *  the window). POSIX: execv. Windows: a new process; the one started by
 *  hand stays as its parent until the last boot ends, so the shell sees one
 *  program that runs until the simulator is closed. */
HOST_NORETURN void host_become(const char *image, char *const argv[], host_link_t *l, host_ctl_t *c);

/* ---- crashes ------------------------------------------------------------- */
/** Call `handler` (which must not return) on a crash in any thread: a bad
 *  access, an illegal instruction, abort(), a stack overflow. `code`: the
 *  signal or exception, for the log. */
void host_on_crash(void (*handler)(int code));
/** From the crash handler: the crashed thread's calls, the innermost first,
 *  one per entry of `out` (each `len` bytes): a function name and offset
 *  ("draw_footer+0x1c") where the computer knows it, else an address.
 *  Returns how many (0 where this isn't supported: Windows for now). */
int host_crash_backtrace(char *out, int max, int len);
/** Call at the start of a thread that may overflow its stack (the
 *  firmware's): leaves room to handle that. */
void host_thread_crash_guard(void);
/** Call `fn` on Ctrl+C or a request to stop (SIGINT, SIGTERM). */
void host_on_quit(void (*fn)(void));

/* ---- TLS ----------------------------------------------------------------- */
/** This computer's trusted root certificates, each as DER, for the
 *  firmware's certificate bundle (esp_crt_bundle). False if it keeps them
 *  only in a PEM file instead (macOS, Linux: /etc/ssl/...). */
bool host_root_certs(void (*fn)(const unsigned char *der, size_t len, void *ctx), void *ctx);

/* ---- mDNS ---------------------------------------------------------------- */
/** Announce a service, e.g. instance "luaboard", type "_luaboard._tcp".
 *  `txt`: n "key=value" strings. Replaces an earlier announcement. False if
 *  the computer has no mDNS to offer (the device is then found by address). */
bool host_mdns_announce(const char *instance, const char *type, uint16_t port, const char *const *txt, size_t n);
void host_mdns_withdraw(void);

#ifdef __cplusplus
}
#endif
