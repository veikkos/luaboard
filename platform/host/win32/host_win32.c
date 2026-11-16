/*
 * The host on Windows (host.h).
 *
 * Serial link and control channel: named pipes, \\.\pipe\<name>. Each has a
 * thread that accepts the PC's connection and reads into a buffer, so the
 * simulator reads with a timeout as it would from a pty; when the PC
 * disconnects the pipe waits for the next one. One client at a time, like a
 * serial port.
 *
 * Reboots: POSIX replaces the process (execv) and keeps its pid; Windows has
 * no such thing. So the executable started by hand stays: it opens the pipes,
 * starts the first boot as a child in a job object and waits until the last
 * process of the job ends, then exits with its code. Every reset starts the
 * next boot (a new process, in the same job) and ends the old one. The pipe
 * handles are inherited by the next boot, and nothing else (Wi-Fi sockets).
 */
#include "posix/posix_compat.h"

#include "host.h"
#include "../host_text.h"

#include <fcntl.h>
#include <process.h>
#include <signal.h>
#include <timeapi.h>
#include <wincrypt.h>
#include <windns.h>

#define CHILD_ENV "LUABOARD_SIM_BOOT"

static void quiet_invalid_parameter(const wchar_t *e, const wchar_t *f, const wchar_t *file, unsigned line,
                                    uintptr_t r)
{
    (void)e; (void)f; (void)file; (void)line; (void)r;
}

void host_init(void)
{
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    /* Sleeps and waits to the millisecond (the firmware's tick is 10 ms).
       Windows 11 may ignore that for a process with no visible window (the
       headless simulator, a minimized one): waits were then 15.6 ms and up,
       and answers on the control channel took 100 ms. Not here. */
    PROCESS_POWER_THROTTLING_STATE pt = { PROCESS_POWER_THROTTLING_CURRENT_VERSION,
                                          PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION, 0 };
    SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &pt, sizeof pt);
    timeBeginPeriod(1);
    /* Files are bytes, as on the device: no CRLF translation; the log too,
       so what reads it sees the same lines everywhere. */
    _set_fmode(_O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
    /* A crash resets the simulated chip; no error dialogs. A CRT call given a
       bad argument fails with EINVAL instead of ending the program. */
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    _set_invalid_parameter_handler(quiet_invalid_parameter);
    SetConsoleOutputCP(CP_UTF8);
}

/* ---- clocks ------------------------------------------------------------ */

int64_t host_mono_us(void)
{
    static LARGE_INTEGER freq;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (int64_t)(c.QuadPart / freq.QuadPart * 1000000 + c.QuadPart % freq.QuadPart * 1000000 / freq.QuadPart);
}

int64_t host_utc_us(void)
{
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    uint64_t t = ((uint64_t)ft.dwHighDateTime << 32 | ft.dwLowDateTime) - 116444736000000000ULL;
    return (int64_t)(t / 10);
}

int64_t host_thread_cpu_us(void)
{
    FILETIME created, exited, kernel, user;
    if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)) return host_mono_us();
    uint64_t k = (uint64_t)kernel.dwHighDateTime << 32 | kernel.dwLowDateTime;
    uint64_t u = (uint64_t)user.dwHighDateTime << 32 | user.dwLowDateTime;
    return (int64_t)((k + u) / 10);
}

void host_sleep_ms(int ms) { Sleep((DWORD)(ms < 0 ? 0 : ms)); }

/* ---- files --------------------------------------------------------------- */

void host_self_path(char *out, size_t cap)
{
    char raw[MAX_PATH] = "";
    DWORD n = GetModuleFileNameA(NULL, raw, sizeof raw);
    if (n == 0 || n >= sizeof raw) raw[0] = 0;
    char full[MAX_PATH];
    snprintf(out, cap, "%s", _fullpath(full, raw, sizeof full) ? full : raw);
}

void host_file_id(const char *path, char *out, size_t cap)
{
    *out = 0;
    /* Not while someone writes it (a linker): it isn't done yet. */
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    /* No file index: the copy host_unlock_self leaves is the same build
       (a copy keeps the size and the time). */
    BY_HANDLE_FILE_INFORMATION fi;
    if (GetFileInformationByHandle(h, &fi)) {
        snprintf(out, cap, "%lx:%lx%08lx:%lx%08lx", fi.dwVolumeSerialNumber, fi.nFileSizeHigh, fi.nFileSizeLow,
                 fi.ftLastWriteTime.dwHighDateTime, fi.ftLastWriteTime.dwLowDateTime);
    }
    CloseHandle(h);
}

static void remove_old_copies(const char *to);

void host_unlock_self(void)
{
    char self[MAX_PATH], copy[MAX_PATH + 40], aside[MAX_PATH + 40];
    host_self_path(self, sizeof self);
    if (!*self) return;
    remove_old_copies(self);                          /* from runs that have ended */
    unsigned long long tick = GetTickCount64();
    snprintf(copy, sizeof copy, "%s.new-%lu-%llu", self, GetCurrentProcessId(), tick);
    snprintf(aside, sizeof aside, "%s.old-%lu-%llu", self, GetCurrentProcessId(), tick);
    /* Swapped in one step, the running file kept as the backup: a
       simulator started from here meanwhile always finds the file. (A
       folder we can't write: it stays locked.) */
    if (!CopyFileA(self, copy, FALSE)) return;
    if (!ReplaceFileA(self, copy, aside, REPLACEFILE_IGNORE_MERGE_ERRORS, NULL, NULL)) DeleteFileA(copy);
}

bool host_set_executable(const char *path, bool on)
{
    (void)on;
    return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
}

bool host_is_executable(const char *path)
{
    DWORD a = GetFileAttributesA(path);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

/* Old copies moved aside because they were running: gone once nothing runs them. */
static void remove_old_copies(const char *to)
{
    char pattern[MAX_PATH + 16], dir[MAX_PATH], path[MAX_PATH * 2];
    snprintf(pattern, sizeof pattern, "%s.old-*", to);
    snprintf(dir, sizeof dir, "%s", to);
    char *slash = strrchr(dir, '\\'), *fwd = strrchr(dir, '/');
    if (fwd > slash) slash = fwd;
    if (slash) slash[1] = 0;
    else dir[0] = 0;
    WIN32_FIND_DATAA fd;
    HANDLE f = FindFirstFileA(pattern, &fd);
    if (f == INVALID_HANDLE_VALUE) return;
    do {
        snprintf(path, sizeof path, "%s%s", dir, fd.cFileName);
        DeleteFileA(path);
    } while (FindNextFileA(f, &fd));
    FindClose(f);
}

bool host_replace_file(const char *from, const char *to)
{
    remove_old_copies(to);
    if (MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING)) return true;
    /* A program that runs can't be replaced, but it can be moved. */
    char aside[MAX_PATH + 32];
    snprintf(aside, sizeof aside, "%s.old-%lu-%llu", to, GetCurrentProcessId(), (unsigned long long)GetTickCount64());
    return MoveFileExA(to, aside, 0) && MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING);
}

bool host_is_program(const unsigned char *head, size_t len) { return len >= 2 && head[0] == 'M' && head[1] == 'Z'; }

/* ---- named pipes with a reader thread ------------------------------------ */

#define RING 65536

typedef struct {
    HANDLE h;
    char name[256];               /* \\.\pipe\... */
    HANDLE thread;
    volatile LONG stop, connected;
    SRWLOCK lock;                 /* the ring */
    uint8_t ring[RING];
    size_t head, len;
    HANDLE data;                  /* manual reset: the ring has bytes */
    HANDLE kick;                  /* auto reset: host_link_kick */
    SRWLOCK wlock;                /* one writer at a time */
    HANDLE wdone;
} pipe_t;

/* NAME, or a path, as a pipe name: \\.\pipe\NAME (\ / : made _). */
static void pipe_name(const char *in, char *out, size_t cap)
{
    static const char prefix[] = "\\\\.\\pipe\\";
    if (!_strnicmp(in, prefix, sizeof prefix - 1) || !_strnicmp(in, "//./pipe/", 9)) in += sizeof prefix - 1;
    int n = snprintf(out, cap, "%s", prefix);
    for (const char *p = in; *p && n + 1 < (int)cap; p++) out[n++] = (*p == '\\' || *p == '/' || *p == ':') ? '_' : *p;
    out[n] = 0;
}

static unsigned __stdcall pipe_reader(void *arg)
{
    pipe_t *p = arg;
    OVERLAPPED ov = { 0 };
    ov.hEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
    uint8_t buf[4096];
    while (!p->stop) {
        if (!p->connected) {
            ResetEvent(ov.hEvent);
            BOOL ok = ConnectNamedPipe(p->h, &ov);
            DWORD e = ok ? 0 : GetLastError(), n;
            if (!ok && e == ERROR_IO_PENDING) ok = GetOverlappedResult(p->h, &ov, &n, TRUE), e = ok ? 0 : GetLastError();
            if (p->stop) break;
            if (!ok && e == ERROR_NO_DATA) { DisconnectNamedPipe(p->h); continue; }   /* a client left before we looked */
            if (!ok && e != ERROR_PIPE_CONNECTED) { Sleep(20); continue; }
            InterlockedExchange(&p->connected, 1);
        }
        AcquireSRWLockShared(&p->lock);
        size_t room = RING - p->len;
        ReleaseSRWLockShared(&p->lock);
        if (!room) { Sleep(1); continue; }            /* nobody reads: let the PC's side fill up */
        ResetEvent(ov.hEvent);
        DWORD n = 0;
        BOOL ok = ReadFile(p->h, buf, (DWORD)(room < sizeof buf ? room : sizeof buf), &n, &ov);
        if (!ok && GetLastError() == ERROR_IO_PENDING) ok = GetOverlappedResult(p->h, &ov, &n, TRUE);
        if (p->stop) break;
        if (!ok) {                                    /* the PC went away: wait for the next one */
            InterlockedExchange(&p->connected, 0);
            DisconnectNamedPipe(p->h);
            continue;
        }
        AcquireSRWLockExclusive(&p->lock);
        for (DWORD i = 0; i < n; i++) p->ring[(p->head + p->len + i) % RING] = buf[i];
        p->len += n;
        if (p->len) SetEvent(p->data);
        ReleaseSRWLockExclusive(&p->lock);
    }
    CloseHandle(ov.hEvent);
    return 0;
}

static pipe_t *pipe_start(HANDLE h, const char *name)
{
    pipe_t *p = calloc(1, sizeof *p);
    if (!p) return NULL;
    p->h = h;
    snprintf(p->name, sizeof p->name, "%s", name);
    InitializeSRWLock(&p->lock);
    InitializeSRWLock(&p->wlock);
    p->data = CreateEventA(NULL, TRUE, FALSE, NULL);
    p->kick = CreateEventA(NULL, FALSE, FALSE, NULL);
    p->wdone = CreateEventA(NULL, TRUE, FALSE, NULL);
    p->thread = (HANDLE)_beginthreadex(NULL, 0, pipe_reader, p, 0, NULL);
    return p;
}

/* `quiet`: say nothing when another simulator has the name (the caller
   tries the next one). */
static pipe_t *pipe_create(const char *name, bool quiet)
{
    char full[256];
    pipe_name(name, full, sizeof full);
    /* Inheritable: the next boot takes it over. */
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    HANDLE h = CreateNamedPipeA(full, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, RING,
                                RING, 0, &sa);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        bool taken = e == ERROR_ACCESS_DENIED || e == ERROR_PIPE_BUSY;
        if (!quiet || !taken)
            fprintf(stderr, "cannot create pipe %s: %s\n", full, taken ? "another simulator has it" : "error");
        return NULL;
    }
    return pipe_start(h, full);
}

/* Stop reading, for the next boot: its own thread reads from here on. */
static void pipe_quiesce(pipe_t *p)
{
    if (!p || !p->thread) return;
    InterlockedExchange(&p->stop, 1);
    CancelIoEx(p->h, NULL);
    WaitForSingleObject(p->thread, 2000);
    CloseHandle(p->thread);
    p->thread = NULL;
}

static int pipe_read(pipe_t *p, void *buf, size_t len, int timeout_ms)
{
    for (int pass = 0; pass < 2; pass++) {
        AcquireSRWLockExclusive(&p->lock);
        size_t n = p->len < len ? p->len : len;
        for (size_t i = 0; i < n; i++) ((uint8_t *)buf)[i] = p->ring[(p->head + i) % RING];
        p->head = (p->head + n) % RING;
        p->len -= n;
        if (!p->len) ResetEvent(p->data);
        ReleaseSRWLockExclusive(&p->lock);
        if (n || !timeout_ms || pass) return (int)n;
        WaitForSingleObject(p->data, timeout_ms < 0 ? INFINITE : (DWORD)timeout_ms);
    }
    return 0;
}

static int pipe_write(pipe_t *p, const void *buf, size_t len, int stall_ms)
{
    if (!p->connected) return (int)len;               /* nobody there: the bytes vanish on the wire */
    AcquireSRWLockExclusive(&p->wlock);
    OVERLAPPED ov = { 0 };
    ov.hEvent = p->wdone;
    ResetEvent(ov.hEvent);
    DWORD n = 0;
    BOOL ok = WriteFile(p->h, buf, (DWORD)len, &n, &ov);
    if (!ok && GetLastError() == ERROR_IO_PENDING) {
        if (WaitForSingleObject(ov.hEvent, stall_ms < 0 ? INFINITE : (DWORD)stall_ms) == WAIT_TIMEOUT)
            CancelIoEx(p->h, &ov);                    /* the PC doesn't read: give up on the rest */
        ok = GetOverlappedResult(p->h, &ov, &n, TRUE);
    }
    DWORD err = ok ? 0 : GetLastError();
    ReleaseSRWLockExclusive(&p->wlock);
    if (!ok && err != ERROR_OPERATION_ABORTED) return (int)len;   /* it left: as if nobody was there */
    return (int)n;
}

/* ---- the serial link ------------------------------------------------------- */

struct host_link {
    pipe_t *p;
};

host_link_t *host_link_create(const char *name, char *where, size_t cap)
{
    pipe_t *p = NULL;
    if (name) {
        p = pipe_create(name, false);
    } else {
        /* The first free of the family: FILE_FLAG_FIRST_PIPE_INSTANCE makes
           taking a name atomic, and a pipe goes with its simulator. */
        char family[32];
        for (int n = 1; n <= HOST_LINK_FAMILY_MAX && !p; n++) {
            host_link_family(n, family, sizeof family);
            p = pipe_create(family, true);
        }
        if (!p) fprintf(stderr, "cannot create pipe: no free simulator name (esp32-sim to esp32-sim-%d taken)\n",
                        HOST_LINK_FAMILY_MAX);
    }
    if (!p) return NULL;
    host_link_t *l = calloc(1, sizeof *l);
    l->p = p;
    snprintf(where, cap, "%s", p->name);
    return l;
}

/* "key=value" in a handover text, copied out. */
static HANDLE handover_handle(const char *h, const char *key)
{
    char v[32];
    if (!host_handover_get(h, key, v, sizeof v)) return NULL;
    return (HANDLE)(uintptr_t)_strtoui64(v, NULL, 10);
}

host_link_t *host_link_adopt(const char *handover)
{
    HANDLE h = handover_handle(handover, "wpipe");
    char name[200] = "esp32-sim", full[256];
    if (!h) return NULL;
    host_handover_get(handover, "wname", name, sizeof name);
    pipe_name(name, full, sizeof full);
    host_link_t *l = calloc(1, sizeof *l);
    l->p = pipe_start(h, full);
    return l;
}

const char *host_link_port(const host_link_t *l) { return l->p->name; }

const char *host_link_name(const host_link_t *l) { return l->p->name; }

int host_link_read(host_link_t *l, void *buf, size_t len, int timeout_ms) { return pipe_read(l->p, buf, len, timeout_ms); }

int host_link_write(host_link_t *l, const void *buf, size_t len, int stall_ms)
{
    return pipe_write(l->p, buf, len, stall_ms);
}

bool host_link_wait(host_link_t *l, int ms, const int *socks, int n)
{
    (void)socks;
    (void)n;
    HANDLE hs[2] = { l->p->data, l->p->kick };
    return WaitForMultipleObjects(2, hs, FALSE, ms < 0 ? INFINITE : (DWORD)ms) == WAIT_OBJECT_0;
}

void host_link_kick(host_link_t *l)
{
    if (l) SetEvent(l->p->kick);
}

void host_link_remove(host_link_t *l) { (void)l; }   /* a pipe goes with its last handle */

/* ---- the control channel: a loopback TCP port ----------------------------
 * --control [HOST:]PORT (port 0: any free one, printed). One client at a
 * time sends lines and reads the answers on the same connection (the
 * end-to-end tests do). Both the listening socket and the open connection
 * go to the next boot, so a client stays connected through a reboot. A
 * thread polls them, so it can stop for the handover without closing them. */

#define CTL_RING 8192

struct host_ctl {
    SOCKET listen, conn;
    HANDLE thread;
    volatile LONG stop;
    SRWLOCK lock;
    char ring[CTL_RING];
    size_t head, len;
    HANDLE data;                  /* manual reset: the ring has bytes */
};

static unsigned __stdcall ctl_reader(void *arg)
{
    host_ctl_t *c = arg;
    char buf[1024];
    while (!c->stop) {
        fd_set rd;
        FD_ZERO(&rd);
        SOCKET s = c->conn != INVALID_SOCKET ? c->conn : c->listen;
        FD_SET(s, &rd);
        /* A short timeout: select() on sockets inherited from the boot
           before doesn't always wake when data comes, only at its timeout
           (measured: 50 ms timeout, 50-150 ms per control line; 5 ms: 5-10).
           It also looks at `stop` for the handover. */
        struct timeval tv = { 0, 2 * 1000 };
        if (select(0, &rd, NULL, NULL, &tv) <= 0) continue;
        if (c->conn == INVALID_SOCKET) {
            SOCKET n = accept(c->listen, NULL, NULL);
            if (n == INVALID_SOCKET) continue;
            /* Lines and answers are tiny: without this, Nagle and the
               client's delayed ACK hold each answer up to 200 ms. */
            BOOL one = TRUE;
            setsockopt(n, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
            SetHandleInformation((HANDLE)n, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
            c->conn = n;
            continue;
        }
        AcquireSRWLockShared(&c->lock);
        size_t room = CTL_RING - c->len;
        ReleaseSRWLockShared(&c->lock);
        if (!room) { Sleep(1); continue; }
        int n = recv(c->conn, buf, (int)(room < sizeof buf ? room : sizeof buf), 0);
        if (n <= 0) {                                 /* the client left: wait for the next */
            closesocket(c->conn);
            c->conn = INVALID_SOCKET;
            continue;
        }
        AcquireSRWLockExclusive(&c->lock);
        for (int i = 0; i < n; i++) c->ring[(c->head + c->len + i) % CTL_RING] = buf[i];
        c->len += (size_t)n;
        SetEvent(c->data);
        ReleaseSRWLockExclusive(&c->lock);
    }
    return 0;
}

static host_ctl_t *ctl_start(SOCKET listen, SOCKET conn)
{
    host_ctl_t *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    c->listen = listen;
    c->conn = conn;
    InitializeSRWLock(&c->lock);
    c->data = CreateEventA(NULL, TRUE, FALSE, NULL);
    c->thread = (HANDLE)_beginthreadex(NULL, 0, ctl_reader, c, 0, NULL);
    return c;
}

host_ctl_t *host_ctl_create(const char *path)
{
    long port = host_ctl_port_parse(path);
    if (port < 0) return NULL;
    /* Inheritable: the next boot takes it over. */
    SOCKET s = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((u_short)port) };
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int alen = sizeof a;
    if (s == INVALID_SOCKET || bind(s, (struct sockaddr *)&a, sizeof a) != 0 || listen(s, 1) != 0 ||
        getsockname(s, (struct sockaddr *)&a, &alen) != 0) {
        fprintf(stderr, "--control %s: cannot listen there (error %d)\n", path, WSAGetLastError());
        if (s != INVALID_SOCKET) closesocket(s);
        return NULL;
    }
    SetHandleInformation((HANDLE)s, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    printf("Control:     127.0.0.1:%u\n", ntohs(a.sin_port));
    return ctl_start(s, INVALID_SOCKET);
}

uint16_t host_ctl_port(const host_ctl_t *c)
{
    struct sockaddr_in a;
    int len = sizeof a;
    return c && getsockname(c->listen, (struct sockaddr *)&a, &len) == 0 ? ntohs(a.sin_port) : 0;
}

host_ctl_t *host_ctl_adopt(const char *handover)
{
    char v[32];
    if (!host_handover_get(handover, "wctl", v, sizeof v)) return NULL;
    SOCKET listen = (SOCKET)_strtoui64(v, NULL, 10), conn = INVALID_SOCKET;
    if (host_handover_get(handover, "wctlc", v, sizeof v)) conn = (SOCKET)_strtoui64(v, NULL, 10);
    host_ctl_t *c = ctl_start(listen, conn);
    /* Lines the boot before had received but not taken: they come first. */
    char hex[2 * 400 + 1];
    if (c && host_handover_get(handover, "wctlbuf", hex, sizeof hex)) {
        AcquireSRWLockExclusive(&c->lock);
        for (const char *p = hex; p[0] && p[1] && c->len < CTL_RING; p += 2) {
            char byte[3] = { p[0], p[1], 0 };
            c->ring[(c->head + c->len++) % CTL_RING] = (char)strtoul(byte, NULL, 16);
        }
        if (c->len) SetEvent(c->data);
        ReleaseSRWLockExclusive(&c->lock);
    }
    return c;
}

int host_ctl_read(host_ctl_t *c, void *buf, size_t len)
{
    AcquireSRWLockExclusive(&c->lock);
    size_t n = c->len < len ? c->len : len;
    for (size_t i = 0; i < n; i++) ((char *)buf)[i] = c->ring[(c->head + i) % CTL_RING];
    c->head = (c->head + n) % CTL_RING;
    c->len -= n;
    if (!c->len) ResetEvent(c->data);
    ReleaseSRWLockExclusive(&c->lock);
    return (int)n;
}

void host_ctl_answer(host_ctl_t *c, const char *text, size_t len)
{
    SOCKET s = c->conn;
    if (s != INVALID_SOCKET) send(s, text, (int)len, 0);   /* lost if nobody listens */
}

void host_ctl_wait(host_ctl_t *c, int ms) { WaitForSingleObject(c->data, ms < 0 ? INFINITE : (DWORD)ms); }

void host_ctl_remove(host_ctl_t *c, const char *path) { (void)c; (void)path; }

/* Stop reading, for the next boot. */
static void ctl_quiesce(host_ctl_t *c)
{
    if (!c || !c->thread) return;
    InterlockedExchange(&c->stop, 1);
    WaitForSingleObject(c->thread, 2000);
    CloseHandle(c->thread);
    c->thread = NULL;
}

/* ---- handover and the next boot -------------------------------------------- */

static const char *short_name(const pipe_t *p) { return p->name + 9; }   /* after \\.\pipe\ */

void host_handover(const host_link_t *l, const host_ctl_t *c, char *out, size_t cap)
{
    int n = snprintf(out, cap, "pty=-1");             /* none: what an older image looks for */
    if (l && n < (int)cap)
        n += snprintf(out + n, cap - n, ",wpipe=%llu,wname=%s", (unsigned long long)(uintptr_t)l->p->h, short_name(l->p));
    if (c && n < (int)cap) {
        /* Stop reading first: what came but wasn't taken yet goes to the next
           boot with the sockets (as unread bytes stay in the socket on POSIX),
           or a client would wait for ever for its answer. */
        ctl_quiesce((host_ctl_t *)c);
        n += snprintf(out + n, cap - n, ",wctl=%llu,wctlc=%llu", (unsigned long long)c->listen, (unsigned long long)c->conn);
        /* (At most 400 bytes, and room left for the simulator's own fields:
           a few lines are all a client sends before it waits for answers.) */
        if (c->len && c->len <= 400 && n + 10 + 2 * (int)c->len < (int)cap - 200) {
            n += snprintf(out + n, cap - n, ",wctlbuf=");
            for (size_t i = 0; i < c->len; i++)
                n += snprintf(out + n, cap - n, "%02x", (unsigned char)c->ring[(c->head + i) % CTL_RING]);
        }
    }
}

/* Append one argument, quoted the way the C runtime splits a command line. */
static void append_arg(char *cmd, size_t cap, const char *a)
{
    size_t n = strlen(cmd);
    if (n && n + 1 < cap) cmd[n++] = ' ';
    bool quote = !*a || strpbrk(a, " \t\"") != NULL;
    if (quote && n + 1 < cap) cmd[n++] = '"';
    for (const char *p = a; *p && n + 3 < cap; p++) {
        size_t bs = 0;
        while (*p == '\\') { bs++; p++; }
        if (!*p) {                                    /* backslashes before the closing quote: doubled */
            for (size_t i = 0; i < bs * (quote ? 2 : 1) && n + 2 < cap; i++) cmd[n++] = '\\';
            break;
        }
        if (*p == '"') {
            for (size_t i = 0; i < bs * 2 + 1 && n + 2 < cap; i++) cmd[n++] = '\\';
        } else {
            for (size_t i = 0; i < bs && n + 2 < cap; i++) cmd[n++] = '\\';
        }
        cmd[n++] = *p;
    }
    if (quote && n + 1 < cap) cmd[n++] = '"';
    cmd[n] = 0;
}

/* Start `image` with only the given handles (and stdio) inherited. */
static HANDLE spawn(const char *image, char *const argv[], HANDLE *handles, int nh, DWORD flags, HANDLE *thread)
{
    static char cmd[32768];
    cmd[0] = 0;
    for (int i = 0; argv[i]; i++) append_arg(cmd, sizeof cmd, argv[i]);

    HANDLE list[8];
    int n = 0;
    for (int i = 0; i < nh; i++) if (handles[i]) list[n++] = handles[i];
    STARTUPINFOEXA si = { 0 };
    si.StartupInfo.cb = sizeof si;
    HANDLE std[3] = { GetStdHandle(STD_INPUT_HANDLE), GetStdHandle(STD_OUTPUT_HANDLE), GetStdHandle(STD_ERROR_HANDLE) };
    bool all = true;
    for (int i = 0; i < 3; i++) {
        if (!std[i] || std[i] == INVALID_HANDLE_VALUE ||
            !SetHandleInformation(std[i], HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT)) { all = false; continue; }
        bool dup = false;
        for (int k = 0; k < n; k++) dup |= list[k] == std[i];
        if (!dup) list[n++] = std[i];
    }
    if (all) {
        si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        si.StartupInfo.hStdInput = std[0];
        si.StartupInfo.hStdOutput = std[1];
        si.StartupInfo.hStdError = std[2];
    }
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(NULL, 1, 0, &size);
    si.lpAttributeList = malloc(size);
    if (!si.lpAttributeList || !InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &size)) return NULL;
    BOOL attr = n == 0 || UpdateProcThreadAttribute(si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, list,
                                                    n * sizeof list[0], NULL, NULL);
    PROCESS_INFORMATION pi;
    BOOL ok = attr && CreateProcessA(image, cmd, NULL, NULL, n > 0, flags | EXTENDED_STARTUPINFO_PRESENT, NULL, NULL,
                                     &si.StartupInfo, &pi);
    DWORD err = GetLastError();
    DeleteProcThreadAttributeList(si.lpAttributeList);
    free(si.lpAttributeList);
    if (!ok) {
        SetLastError(err);
        return NULL;
    }
    /* Suspended: the caller resumes it (after putting it in the job). */
    if (thread) *thread = pi.hThread;
    else CloseHandle(pi.hThread);
    return pi.hProcess;
}

static BOOL WINAPI supervisor_ctrl(DWORD type)
{
    (void)type;
    return TRUE;                                      /* the boot running decides what Ctrl+C means */
}

/* The executable started by hand: run the boots, one after the other, and
   end with the last one. */
static HOST_NORETURN void supervise(const char *image, char *const argv[], HANDLE *handles, int nh)
{
    HANDLE job = CreateJobObjectA(NULL, NULL);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION lim = { 0 };
    lim.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;   /* we go, the boots go */
    SetInformationJobObject(job, JobObjectExtendedLimitInformation, &lim, sizeof lim);
    HANDLE port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 1);
    JOBOBJECT_ASSOCIATE_COMPLETION_PORT acp = { job, port };
    SetInformationJobObject(job, JobObjectAssociateCompletionPortInformation, &acp, sizeof acp);
    SetConsoleCtrlHandler(supervisor_ctrl, TRUE);
    SetEnvironmentVariableA(CHILD_ENV, "1");

    HANDLE thread = NULL;
    HANDLE last = spawn(image, argv, handles, nh, CREATE_SUSPENDED, &thread);
    if (!last) {
        fprintf(stderr, "cannot start %s (error %lu)\n", image, GetLastError());
        ExitProcess(1);
    }
    AssignProcessToJobObject(job, last);
    ResumeThread(thread);
    CloseHandle(thread);

    DWORD code = 0;
    for (;;) {
        DWORD msg;
        ULONG_PTR key;
        LPOVERLAPPED data;
        if (!GetQueuedCompletionStatus(port, &msg, &key, &data, INFINITE)) break;
        if (msg == JOB_OBJECT_MSG_NEW_PROCESS) {
            HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)(uintptr_t)data);
            if (h) { CloseHandle(last); last = h; }
        } else if (msg == JOB_OBJECT_MSG_ACTIVE_PROCESS_ZERO) {
            break;
        }
    }
    WaitForSingleObject(last, INFINITE);
    GetExitCodeProcess(last, &code);
    ExitProcess(code);
}

void host_become(const char *image, char *const argv[], host_link_t *l, host_ctl_t *c)
{
    fflush(stdout);
    fflush(stderr);
    /* The pipes go to the next boot whole: no read of ours may take its bytes. */
    pipe_quiesce(l ? l->p : NULL);
    ctl_quiesce(c);
    HANDLE hs[3] = { l ? l->p->h : NULL, c ? (HANDLE)c->listen : NULL, c && c->conn != INVALID_SOCKET ? (HANDLE)c->conn : NULL };
    if (!GetEnvironmentVariableA(CHILD_ENV, NULL, 0)) supervise(image, argv, hs, 3);
    HANDLE p = spawn(image, argv, hs, 3, 0, NULL);
    if (!p) {
        fprintf(stderr, "cannot start %s (error %lu)\n", image, GetLastError());
        ExitProcess(1);
    }
    CloseHandle(p);
    ExitProcess(0);
}

/* ---- crashes ------------------------------------------------------------- */

static void (*s_crash)(int);

static LONG WINAPI on_exception(EXCEPTION_POINTERS *e)
{
    if (s_crash) s_crash((int)e->ExceptionRecord->ExceptionCode);
    return EXCEPTION_EXECUTE_HANDLER;
}

static void on_abort(int sig) { if (s_crash) s_crash(sig); }

int host_crash_backtrace(char *out, int max, int len)
{
    (void)out; (void)max; (void)len;             /* TODO: StackWalk64 from the exception's context */
    return 0;
}

void host_on_crash(void (*handler)(int code))
{
    s_crash = handler;
    SetUnhandledExceptionFilter(on_exception);
    signal(SIGABRT, on_abort);
}

void host_thread_crash_guard(void)
{
    /* Room on the stack to handle its overflow; pthread_create reserved it
       on top of the stack the thread asked for. */
    ULONG guarantee = HOST_CRASH_GUARD_BYTES;
    if (!SetThreadStackGuarantee(&guarantee))
        fprintf(stderr, "warning: no stack set aside for a crash (%lu)\n", GetLastError());
}

static void (*s_quit)(void);

static BOOL WINAPI on_ctrl(DWORD type)
{
    (void)type;
    if (s_quit) s_quit();
    return TRUE;
}

void host_on_quit(void (*fn)(void))
{
    s_quit = fn;
    SetConsoleCtrlHandler(on_ctrl, TRUE);
}

/* ---- TLS: the trusted roots of Windows' own store ----------------------- */

bool host_root_certs(void (*fn)(const unsigned char *der, size_t len, void *ctx), void *ctx)
{
    HCERTSTORE store = CertOpenSystemStoreA(0, "ROOT");
    if (!store) return false;
    for (PCCERT_CONTEXT c = NULL; (c = CertEnumCertificatesInStore(store, c));)
        fn(c->pbCertEncoded, c->cbCertEncoded, ctx);
    CertCloseStore(store, 0);
    return true;
}

/* ---- mDNS: Windows' own DNS-SD (Windows 10 1809 and later) -------------- */

/* Registering and deregistering finish later, on a thread of Windows', and
 * Windows keeps using the request and the instance it points to until then.
 * So each request lives on the heap until its completion callback, and the
 * instance is counted: the announcement holds it once, each pending request
 * once, and the last one to let go frees it. */

typedef struct {
    PDNS_SERVICE_INSTANCE inst;
    volatile LONG refs;
} mdns_inst_t;

typedef struct {
    DNS_SERVICE_REGISTER_REQUEST req;
    mdns_inst_t *inst;
    bool deregister;
} mdns_op_t;

static mdns_inst_t *s_inst; /* what is announced */

static void mdns_release(mdns_inst_t *mi)
{
    if (InterlockedDecrement(&mi->refs) == 0) {
        DnsServiceFreeInstance(mi->inst);
        free(mi);
    }
}

static VOID WINAPI mdns_done(DWORD status, PVOID ctx, PDNS_SERVICE_INSTANCE inst)
{
    mdns_op_t *op = ctx;
    if (status != ERROR_SUCCESS)
        fprintf(stderr, "mdns: %s failed (%lu)\n", op->deregister ? "deregistration" : "registration", status);
    if (inst) DnsServiceFreeInstance(inst); /* a copy of its own, for the callback */
    mdns_release(op->inst);
    free(op);
}

/* Starts registering or deregistering mi; false if it didn't start. */
static bool mdns_start(mdns_inst_t *mi, bool deregister)
{
    mdns_op_t *op = calloc(1, sizeof *op);
    if (!op) return false;
    op->req.Version = DNS_QUERY_REQUEST_VERSION1;
    op->req.pServiceInstance = mi->inst;
    op->req.pRegisterCompletionCallback = mdns_done;
    op->req.pQueryContext = op;
    op->inst = mi;
    op->deregister = deregister;
    InterlockedIncrement(&mi->refs);
    DWORD r = deregister ? DnsServiceDeRegister(&op->req, NULL) : DnsServiceRegister(&op->req, NULL);
    if (r != DNS_REQUEST_PENDING) {
        mdns_release(mi);
        free(op);
        return false;
    }
    return true;
}

static void widen(const char *s, wchar_t *out, int cap) { MultiByteToWideChar(CP_UTF8, 0, s, -1, out, cap); }

bool host_mdns_announce(const char *instance, const char *type, uint16_t port, const char *const *txt, size_t n)
{
    host_mdns_withdraw();
    wchar_t service[256], host[300], keys[8][64], values[8][128];
    char name[300], computer[256] = "localhost";
    DWORD len = sizeof computer;
    GetComputerNameExA(ComputerNameDnsHostname, computer, &len);
    snprintf(name, sizeof name, "%s.%s.local", instance, type);
    widen(name, service, 256);
    snprintf(name, sizeof name, "%s.local", computer);
    widen(name, host, 300);
    PCWSTR kp[8], vp[8];
    if (n > 8) n = 8;
    for (size_t i = 0; i < n; i++) {
        const char *eq = strchr(txt[i], '=');
        char k[64];
        snprintf(k, sizeof k, "%.*s", eq ? (int)(eq - txt[i]) : (int)strlen(txt[i]), txt[i]);
        widen(k, keys[i], 64);
        widen(eq ? eq + 1 : "", values[i], 128);
        kp[i] = keys[i];
        vp[i] = values[i];
    }
    mdns_inst_t *mi = calloc(1, sizeof *mi);
    if (!mi) return false;
    mi->inst = DnsServiceConstructInstance(service, host, NULL, NULL, port, 0, 0, (DWORD)n, kp, vp);
    if (!mi->inst) {
        free(mi);
        return false;
    }
    mi->refs = 1;
    if (!mdns_start(mi, false)) {
        mdns_release(mi);
        return false;
    }
    s_inst = mi;
    return true;
}

void host_mdns_withdraw(void)
{
    if (!s_inst) return;
    mdns_start(s_inst, true); /* if it doesn't start there's nothing more to do */
    mdns_release(s_inst);
    s_inst = NULL;
}
