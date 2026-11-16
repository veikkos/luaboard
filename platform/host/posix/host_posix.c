/*
 * The host on macOS and Linux (host.h): the serial link is a pseudo-terminal,
 * the control channel a loopback TCP port, and a boot hands them to the next
 * one by descriptor through execv.
 */
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#elif !defined(_GNU_SOURCE)              /* (the build defines it on Linux) */
#define _GNU_SOURCE
#endif
#include "host.h"
#include "../host_text.h"

#include <errno.h>
#include <execinfo.h>
#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#ifdef __APPLE__
#include <Security/Security.h>
#include <mach-o/dyld.h>
#endif
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#ifdef HOST_HAVE_DNS_SD
#include <dns_sd.h>
#endif

void host_init(void)
{
    /* lwIP has no SIGPIPE; on the host a write to a closed TCP client would kill us. */
    signal(SIGPIPE, SIG_IGN);
}

/* ---- clocks ------------------------------------------------------------ */

int64_t host_mono_us(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}

int64_t host_thread_cpu_us(void)
{
    struct timespec t;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
    return (int64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}

int64_t host_utc_us(void)
{
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    return (int64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}

void host_sleep_ms(int ms)
{
    struct timespec t = { ms / 1000, (long)(ms % 1000) * 1000000 };
    while (nanosleep(&t, &t) != 0 && errno == EINTR) {}
}

/* ---- files --------------------------------------------------------------- */

void host_self_path(char *out, size_t cap)
{
    char raw[PATH_MAX] = "";
#ifdef __APPLE__
    uint32_t n = sizeof raw;
    if (_NSGetExecutablePath(raw, &n) != 0) raw[0] = 0;
#else
    ssize_t n = readlink("/proc/self/exe", raw, sizeof raw - 1);
    raw[n > 0 ? n : 0] = 0;
#endif
    char real[PATH_MAX];
    snprintf(out, cap, "%s", realpath(raw, real) ? real : raw);
}

void host_unlock_self(void) {}   /* a build replaces the file (a new inode); the running one stays */

void host_file_id(const char *path, char *out, size_t cap)
{
    struct stat st;
    if (stat(path, &st) != 0) { *out = 0; return; }
#ifdef __APPLE__
    long ns = st.st_mtimespec.tv_nsec;
#else
    long ns = st.st_mtim.tv_nsec;
#endif
    snprintf(out, cap, "%llu:%llu:%lld:%ld.%09ld", (unsigned long long)st.st_dev, (unsigned long long)st.st_ino,
             (long long)st.st_size, (long)st.st_mtime, ns);
}

bool host_set_executable(const char *path, bool on) { return chmod(path, on ? 0755 : 0644) == 0; }

bool host_is_executable(const char *path) { return access(path, X_OK) == 0; }

/* A rename: the process doing it may be running the old file, whose pages must stay. */
bool host_replace_file(const char *from, const char *to) { return rename(from, to) == 0; }

bool host_is_program(const unsigned char *head, size_t len)
{
    static const unsigned char macho[4] = { 0xcf, 0xfa, 0xed, 0xfe }, elf[4] = { 0x7f, 'E', 'L', 'F' };
    return len >= 4 && (!memcmp(head, macho, 4) || !memcmp(head, elf, 4));
}

/* ---- the serial link: a pty ------------------------------------------- */

struct host_link {
    int pty, pts;                 /* master, and our own handle to the slave */
    int kick[2];                  /* self-pipe: host_link_kick */
    int lock;                     /* flock on <name>.lock while we have the link; -1: none */
    char port[128];
    char name[256];               /* the symlink we made; "": none */
};

static void nonblocking(int fd)
{
    int fl = fcntl(fd, F_GETFL);
    if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

static host_link_t *link_new(int pty, int pts)
{
    host_link_t *l = calloc(1, sizeof *l);
    if (!l) return NULL;
    l->pty = pty;
    l->pts = pts;
    l->kick[0] = l->kick[1] = -1;
    l->lock = -1;
    if (pipe(l->kick) == 0) {
        nonblocking(l->kick[0]);
        nonblocking(l->kick[1]);
        fcntl(l->kick[0], F_SETFD, FD_CLOEXEC);
        fcntl(l->kick[1], F_SETFD, FD_CLOEXEC);
    }
    nonblocking(pty);
    const char *name = ptsname(pty);
    snprintf(l->port, sizeof l->port, "%s", name ? name : "?");
    return l;
}

/* Make `path` our link, if no simulator has it. Who has a name is told by
   an advisory lock on <path>.lock, held for the simulator's life (it goes
   on through the resets with the pty, host_become): the system lets go of
   it when a simulator dies, so a leftover link of a crashed one is free.
   The lock files stay behind: removing one while another simulator opens
   it would let two think they have the name. */
static bool link_claim(host_link_t *l, const char *path, bool quiet)
{
    char lock[sizeof l->name + 8];
    if (snprintf(lock, sizeof lock, "%s.lock", path) >= (int)sizeof lock || strlen(path) >= sizeof l->name) {
        if (!quiet) fprintf(stderr, "warning: cannot create link %s: the path is too long\n", path);
        return false;
    }
    /* Anyone can make files in /tmp: the lock must be a plain file of ours,
       not a link to somewhere else (O_NOFOLLOW) or a FIFO (O_NONBLOCK). */
    int fd = open(lock, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
    if (fd < 0) {
        if (!quiet) fprintf(stderr, "warning: cannot create link %s: %s: %s\n", path, lock, strerror(errno));
        return false;
    }
    struct stat ls;
    if (fstat(fd, &ls) != 0 || !S_ISREG(ls.st_mode) || ls.st_uid != getuid()) {
        if (!quiet)
            fprintf(stderr, "warning: cannot create link %s: %s isn't a plain file of this user's\n", path, lock);
        close(fd);
        return false;
    }
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        if (!quiet) fprintf(stderr, "warning: cannot create link %s: another simulator has it\n", path);
        close(fd);
        return false;
    }
    struct stat st;
    if (lstat(path, &st) == 0 && S_ISLNK(st.st_mode)) unlink(path);   /* nobody's: its lock was free */
    if (symlink(l->port, path) != 0) {
        if (!quiet) fprintf(stderr, "warning: cannot create link %s: %s\n", path, strerror(errno));
        close(fd);                                                     /* and with it the lock */
        return false;
    }
    l->lock = fd;
    snprintf(l->name, sizeof l->name, "%s", path);
    return true;
}

host_link_t *host_link_create(const char *name, char *where, size_t cap)
{
    /* macOS's grantpt can fail now and then when many processes open a pty
       at the same moment (the tests start dozens of simulators at once):
       try again for a moment. */
    int m = -1;
    for (int tries = 0; tries < 50; tries++) {
        m = posix_openpt(O_RDWR | O_NOCTTY);
        if (m >= 0 && grantpt(m) == 0 && unlockpt(m) == 0) break;
        if (m >= 0) close(m);
        m = -1;
        nanosleep(&(struct timespec){ .tv_nsec = 10 * 1000 * 1000 }, NULL);
    }
    if (m < 0) { perror("posix_openpt"); return NULL; }
    const char *sp = ptsname(m);
    if (!sp) { perror("ptsname"); return NULL; }
    /* Keep one handle to the slave open ourselves: otherwise the master sees
       hang-ups whenever the PC tool disconnects. Set it raw (no echo, no CRLF
       translation); the client applies its own settings on open anyway. */
    int s = open(sp, O_RDWR | O_NOCTTY);
    if (s < 0) { perror("open pty slave"); return NULL; }
    struct termios t;
    if (tcgetattr(s, &t) == 0) {
        cfmakeraw(&t);
        tcsetattr(s, TCSANOW, &t);
    }
    host_link_t *l = link_new(m, s);
    if (!l) return NULL;
    if (name) {
        link_claim(l, name, false);
    } else {
        char path[64], family[32];
        for (int n = 1; n <= HOST_LINK_FAMILY_MAX && !*l->name; n++) {
            host_link_family(n, family, sizeof family);
            snprintf(path, sizeof path, "/tmp/%s", family);
            link_claim(l, path, true);
        }
        if (!*l->name) fprintf(stderr, "warning: no free simulator link (/tmp/esp32-sim to /tmp/esp32-sim-%d taken)\n",
                               HOST_LINK_FAMILY_MAX);
    }
    if (*l->name) snprintf(where, cap, "%s  (link: %s)", l->port, l->name);
    else snprintf(where, cap, "%s", l->port);
    return l;
}

/* "key=N" in a handover text. */
static int handover_int(const char *h, const char *key)
{
    char v[24];
    return host_handover_get(h, key, v, sizeof v) ? atoi(v) : -1;
}

host_link_t *host_link_adopt(const char *handover)
{
    int pty = handover_int(handover, "pty"), pts = handover_int(handover, "pts");
    host_link_t *l = pty >= 0 ? link_new(pty, pts) : NULL;
    if (l) {
        l->lock = handover_int(handover, "lock");
        if (l->lock >= 0) host_handover_get(handover, "lname", l->name, sizeof l->name);
    }
    return l;
}

const char *host_link_port(const host_link_t *l) { return l->port; }

const char *host_link_name(const host_link_t *l) { return *l->name ? l->name : NULL; }

int host_link_read(host_link_t *l, void *buf, size_t len, int timeout_ms)
{
    if (timeout_ms == 0) {
        /* What's there now: the read says so by itself (the pty doesn't
           block), at a fraction of what asking first costs. The firmware
           looks every pass of its main loop, and in a fast-forward that was
           most of a pass. */
        ssize_t n = read(l->pty, buf, len);
        return n > 0 ? (int)n : (n == 0 || errno == EAGAIN || errno == EIO) ? 0 : -1;
    }
    struct pollfd pfd = { .fd = l->pty, .events = POLLIN };
    int r = poll(&pfd, 1, timeout_ms);
    if (r <= 0) return 0;
    /* POLLHUP with no data: nobody has the other side open. */
    if (!(pfd.revents & POLLIN)) return timeout_ms != 0 ? HOST_LINK_HUNG_UP : 0;
    ssize_t n = read(l->pty, buf, len);
    if (n < 0) return (errno == EAGAIN || errno == EIO) ? 0 : -1;
    return (int)n;
}

int host_link_write(host_link_t *l, const void *buf, size_t len, int stall_ms)
{
    const uint8_t *p = buf;
    size_t left = len;
    int64_t stalled_since = 0;                           /* 0: the last write got through */
    while (left > 0) {
        ssize_t n = write(l->pty, p, left);
        if (n > 0) { p += n; left -= (size_t)n; stalled_since = 0; continue; }
        if (n < 0 && errno == EAGAIN && stall_ms != 0) {
            /* Full: wait for the reader. poll() alone isn't enough: on
               macOS it calls a pty writable that takes nothing yet, and
               the loop spun at full speed until the reader had caught
               up, taking the processor the reader needed (a 150 KB frame:
               15 ms alone, 600 ms with a few simulators doing the same). */
            int64_t now = host_mono_us();
            if (!stalled_since) stalled_since = now;
            if (stall_ms > 0 && now - stalled_since >= (int64_t)stall_ms * 1000) break;
            struct pollfd pfd = { .fd = l->pty, .events = POLLOUT };
            int r = poll(&pfd, 1, stall_ms);
            if (r > 0 && (pfd.revents & POLLOUT)) {
                if (now != stalled_since) host_sleep_ms(1);   /* said writable before, and wasn't */
                continue;
            }
        }
        break;
    }
    return (int)(len - left);
}

bool host_link_wait(host_link_t *l, int ms, const int *socks, int n)
{
    enum { SOCKS_MAX = 16 };
    struct pollfd p[2 + SOCKS_MAX] = { { .fd = l->pty, .events = POLLIN }, { .fd = l->kick[0], .events = POLLIN } };
    if (n > SOCKS_MAX) n = SOCKS_MAX;
    for (int i = 0; i < n; i++) p[2 + i] = (struct pollfd){ .fd = socks[i], .events = POLLIN };
    if (l->kick[0] < 0) p[1].fd = -1;                /* (ignored by poll) */
    if (poll(p, (nfds_t)(2 + n), ms) <= 0) return false;
    if (p[1].revents & POLLIN) {
        char drain[64];
        while (read(l->kick[0], drain, sizeof drain) > 0) {}
    }
    bool input = (p[0].revents & POLLIN) != 0;
    for (int i = 0; i < n; i++) input = input || p[2 + i].revents != 0;   /* (closed counts: the read says so) */
    return input;
}

void host_link_kick(host_link_t *l)
{
    if (l && l->kick[1] >= 0) (void)!write(l->kick[1], "!", 1);
}

void host_link_remove(host_link_t *l)
{
    /* Only while it is still ours (it is, while we hold its lock; checked
       anyway). The lock goes with the process. */
    char to[sizeof l->port];
    if (!l || !*l->name) return;
    ssize_t n = readlink(l->name, to, sizeof to - 1);
    if (n < 0) return;
    to[n] = 0;
    if (!strcmp(to, l->port)) unlink(l->name);
}

/* ---- the control channel: a loopback TCP port ----------------------------
 * --control [127.0.0.1:]PORT (0: any free one, printed). One client at a
 * time sends lines and reads the answers on the same connection; the next
 * one waits until it leaves. Both sockets go to the next boot (execv keeps
 * them), and so do lines not read yet: they wait in the socket. */

struct host_ctl {
    int listen, conn;
};

static void ctl_accept(host_ctl_t *c)
{
    if (c->conn >= 0) return;
    int n = accept(c->listen, NULL, NULL);
    if (n < 0) return;
    nonblocking(n);
    /* Lines and answers are tiny: without this, Nagle and the client's
       delayed ACK hold an answer up to 200 ms. */
    int one = 1;
    setsockopt(n, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    c->conn = n;
}

static host_ctl_t *ctl_new(int listen, int conn)
{
    host_ctl_t *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    c->listen = listen;
    c->conn = conn;
    return c;
}

host_ctl_t *host_ctl_create(const char *path)
{
    long port = host_ctl_port_parse(path);
    if (port < 0) return NULL;
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port) };
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t alen = sizeof a;
    int one = 1;
    if (s >= 0) setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    if (s < 0 || bind(s, (struct sockaddr *)&a, sizeof a) != 0 || listen(s, 8) != 0 ||
        getsockname(s, (struct sockaddr *)&a, &alen) != 0) {
        fprintf(stderr, "--control %s: cannot listen there: %s\n", path, strerror(errno));
        if (s >= 0) close(s);
        return NULL;
    }
    nonblocking(s);
    printf("Control:     127.0.0.1:%u\n", ntohs(a.sin_port));
    fflush(stdout);
    return ctl_new(s, -1);
}

uint16_t host_ctl_port(const host_ctl_t *c)
{
    struct sockaddr_in a;
    socklen_t len = sizeof a;
    return c && getsockname(c->listen, (struct sockaddr *)&a, &len) == 0 ? ntohs(a.sin_port) : 0;
}

host_ctl_t *host_ctl_adopt(const char *handover)
{
    int listen = handover_int(handover, "tctl");
    return listen >= 0 ? ctl_new(listen, handover_int(handover, "tctlc")) : NULL;
}

int host_ctl_read(host_ctl_t *c, void *buf, size_t len)
{
    ctl_accept(c);
    if (c->conn < 0) return 0;
    ssize_t n = recv(c->conn, buf, len, 0);
    if (n > 0) return (int)n;
    if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
        close(c->conn);                               /* the client left: wait for the next */
        c->conn = -1;
    }
    return 0;
}

void host_ctl_answer(host_ctl_t *c, const char *text, size_t len)
{
    if (c->conn >= 0) (void)!send(c->conn, text, len, 0);   /* lost if nobody listens */
}

void host_ctl_wait(host_ctl_t *c, int ms)
{
    struct pollfd p = { .fd = c->conn >= 0 ? c->conn : c->listen, .events = POLLIN };
    poll(&p, 1, ms);
}

void host_ctl_remove(host_ctl_t *c, const char *path) { (void)c; (void)path; }   /* the sockets go with the process */

/* ---- handover -------------------------------------------------------------- */

void host_handover(const host_link_t *l, const host_ctl_t *c, char *out, size_t cap)
{
    /* pty and pts as older images know them (a rollback may hand over to
       one); their control channel was FIFOs, so they go on without one. */
    int n = snprintf(out, cap, "pty=%d,pts=%d,tctl=%d,tctlc=%d", l ? l->pty : -1, l ? l->pts : -1,
                     c ? c->listen : -1, c ? c->conn : -1);
    /* The link's name and its lock go along, so the next boot keeps the
       name and removes the link at the end. A name with a comma can't be in
       the handover (it splits on commas): that link stays behind at exit. */
    if (l && l->lock >= 0 && n >= 0 && n < (int)cap) {
        n += snprintf(out + n, cap - n, ",lock=%d", l->lock);
        if (*l->name && !strchr(l->name, ',') && n < (int)cap) snprintf(out + n, cap - n, ",lname=%s", l->name);
    }
}

/* The descriptor goes on into the next image if it's one of keep[KEEP], else
   it's closed at the exec. */
#define KEEP 5
static void exec_keeps(int fd, const int keep[KEEP])
{
    bool kept = false;
    for (int i = 0; i < KEEP; i++) kept |= keep[i] == fd;
    int fl = fcntl(fd, F_GETFD);
    if (fl >= 0) fcntl(fd, F_SETFD, kept ? fl & ~FD_CLOEXEC : fl | FD_CLOEXEC);
}

void host_become(const char *image, char *const argv[], host_link_t *l, host_ctl_t *c)
{
    int keep[KEEP] = { l ? l->pty : -1, l ? l->pts : -1, c ? c->listen : -1, c ? c->conn : -1, l ? l->lock : -1 };
    /* /dev/fd lists the open descriptors (macOS, Linux, the BSDs). Without
       it, every number up to the limit: a system call for each, which with
       many simulators starting at once kept the machine busy for seconds. */
    DIR *d = opendir("/dev/fd");
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            char *end;
            long fd = strtol(e->d_name, &end, 10);
            if (end != e->d_name && !*end && fd >= 3 && fd != dirfd(d)) exec_keeps((int)fd, keep);
        }
        closedir(d);
    } else {
        int max = (int)sysconf(_SC_OPEN_MAX);
        if (max < 0 || max > 65536) max = 65536;
        for (int fd = 3; fd < max; fd++) exec_keeps(fd, keep);
    }
    fflush(stdout);
    fflush(stderr);
    execv(image, argv);
    fprintf(stderr, "cannot start %s: %s\n", image, strerror(errno));
    _exit(1);
}

/* ---- crashes ------------------------------------------------------------- */

static void (*s_crash)(int);

static void on_crash(int sig) { s_crash(sig); }

int host_crash_backtrace(char *out, int max, int len)
{
    void *pcs[64];
    int n = backtrace(pcs, 64), first = 0, k = 0;
    /* Skip the handler's own frames: up to the signal trampoline, after
       which comes the call that crashed. */
    for (int i = 0; i < n; i++) {
        Dl_info d;
        if (dladdr(pcs[i], &d) && d.dli_sname && strstr(d.dli_sname, "sigtramp")) first = i + 1;
    }
    for (int i = first; i < n && k < max; i++, k++) {
        Dl_info d;
        char *dst = out + (size_t)k * (size_t)len;
        if (dladdr(pcs[i], &d) && d.dli_sname)
            snprintf(dst, (size_t)len, "%s+%#lx", d.dli_sname, (unsigned long)((char *)pcs[i] - (char *)d.dli_saddr));
        else
            snprintf(dst, (size_t)len, "%p", pcs[i]);
    }
    return k;
}

void host_on_crash(void (*handler)(int code))
{
    s_crash = handler;
    struct sigaction sa = { .sa_handler = on_crash, .sa_flags = SA_NODEFER | SA_ONSTACK };
    sigemptyset(&sa.sa_mask);
    int sigs[] = { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT };
    for (size_t i = 0; i < sizeof sigs / sizeof sigs[0]; i++) sigaction(sigs[i], &sa, NULL);
}

void host_thread_crash_guard(void)
{
    /* A crash handler's own stack: one that overflowed the thread's stack
       still gets to reset the chip. */
    static char alt[64 * 1024];
    stack_t ss = { .ss_sp = alt, .ss_size = sizeof alt };
    if (sigaltstack(&ss, NULL) != 0) perror("sigaltstack");
}

static void (*s_quit)(void);

static void on_quit(int sig) { (void)sig; s_quit(); }

void host_on_quit(void (*fn)(void))
{
    s_quit = fn;
    signal(SIGINT, on_quit);
    signal(SIGTERM, on_quit);
}

/* ---- TLS ------------------------------------------------------------------- */

#ifdef __APPLE__
/* The system's trusted roots from the keychain. /etc/ssl/cert.pem lags behind
 * it (it lacks GlobalSign Root R46, which BMW's CarData stream needs). */
bool host_root_certs(void (*fn)(const unsigned char *der, size_t len, void *ctx), void *ctx)
{
    CFArrayRef anchors = NULL;
    if (SecTrustCopyAnchorCertificates(&anchors) != errSecSuccess || !anchors) return false;
    for (CFIndex i = 0; i < CFArrayGetCount(anchors); i++) {
        SecCertificateRef cert = (SecCertificateRef)CFArrayGetValueAtIndex(anchors, i);
        CFDataRef der = SecCertificateCopyData(cert);
        if (!der) continue;
        fn(CFDataGetBytePtr(der), (size_t)CFDataGetLength(der), ctx);
        CFRelease(der);
    }
    CFRelease(anchors);
    return true;
}
#else
bool host_root_certs(void (*fn)(const unsigned char *der, size_t len, void *ctx), void *ctx)
{
    (void)fn; (void)ctx;
    return false;                     /* a PEM bundle in /etc/ssl (mock_crt_bundle.c) */
}
#endif

/* ---- mDNS ------------------------------------------------------------------ */

#ifdef HOST_HAVE_DNS_SD
static DNSServiceRef s_dns;

bool host_mdns_announce(const char *instance, const char *type, uint16_t port, const char *const *txt, size_t n)
{
    TXTRecordRef rec;
    char buf[256];
    TXTRecordCreate(&rec, sizeof buf, buf);
    for (size_t i = 0; i < n; i++) {
        const char *eq = strchr(txt[i], '=');
        char key[64];
        snprintf(key, sizeof key, "%.*s", eq ? (int)(eq - txt[i]) : (int)strlen(txt[i]), txt[i]);
        const char *v = eq ? eq + 1 : "";
        TXTRecordSetValue(&rec, key, (uint8_t)strlen(v), v);
    }
    host_mdns_withdraw();
    DNSServiceErrorType e = DNSServiceRegister(&s_dns, 0, 0, instance, type, NULL, NULL, htons(port),
                                               TXTRecordGetLength(&rec), TXTRecordGetBytesPtr(&rec), NULL, NULL);
    TXTRecordDeallocate(&rec);
    if (e != kDNSServiceErr_NoError) {
        s_dns = NULL;
        fprintf(stderr, "mdns: Bonjour registration failed (%d)\n", (int)e);
        return false;
    }
    return true;
}

void host_mdns_withdraw(void)
{
    if (s_dns) DNSServiceRefDeallocate(s_dns);
    s_dns = NULL;
}
#else
bool host_mdns_announce(const char *instance, const char *type, uint16_t port, const char *const *txt, size_t n)
{
    (void)instance; (void)type; (void)port; (void)txt; (void)n;
    return false;
}

void host_mdns_withdraw(void) {}
#endif
