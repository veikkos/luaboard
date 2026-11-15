/*
 * The POSIX the firmware and the mock expect, on Windows (MSVC).
 *
 * On the device ESP-IDF gives the firmware newlib, pthreads and a VFS with
 * POSIX file calls; on macOS and Linux the simulator gets them from the
 * system. On Windows this layer stands in: headers MSVC lacks live next to
 * this one (pthread.h, unistd.h, dirent.h, sys/time.h, strings.h), and this
 * header, forced into every source of ours (/FI), covers what MSVC has under
 * other names or with other behaviour:
 *   - rename() replaces an existing file, as POSIX's does
 *   - mkdir() takes a mode
 *   - localtime_r() and tzset() follow a POSIX TZ string (EET-2EEST,M3.5.0/3,
 *     M10.5.0/4), which the firmware sets for its time zone; MSVC's tzset()
 *     only knows "EST5EDT" with US rules
 *   - GCC attributes and builtins the sources use
 * Implementations: posix_win32.c, pthread_win32.c, tz_win32.c.
 */
#pragma once
#ifndef _WIN32
#error "posix_compat.h is for Windows builds only"
#endif

/* The CRT's POSIX names (close, unlink, ...), even in strict C modes. */
#ifndef _CRT_DECLARE_NONSTDC_NAMES
#define _CRT_DECLARE_NONSTDC_NAMES 1
#endif
#ifndef _CRT_NONSTDC_NO_DEPRECATE
#define _CRT_NONSTDC_NO_DEPRECATE
#endif
/* Winsock before windows.h, or windows.h drags in the old winsock.h. */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
/* Winsock's old in_addr field names are macros; they would rename any
   variable called s_host or s_net. */
#undef s_host
#undef s_net
#undef s_imp
#undef s_impno
#undef s_lh

#include <direct.h>
#include <errno.h>
#include <io.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- compiler -------------------------------------------------------- */
#ifdef _MSC_VER
#ifndef __attribute__
#define __attribute__(x)
#endif
#include <intrin.h>
#define __builtin_frame_address(level) ((void *)_AddressOfReturnAddress())
#endif

/* ---- types and limits ------------------------------------------------ */
#ifndef _SSIZE_T_DEFINED
#define _SSIZE_T_DEFINED
typedef intptr_t ssize_t;
#endif
typedef long suseconds_t;
typedef unsigned int useconds_t;
#ifndef PATH_MAX
#define PATH_MAX 1024
#endif

/* ---- strings --------------------------------------------------------- */
#define strcasecmp _stricmp
#define strncasecmp _strnicmp
#define strtok_r strtok_s
char *strndup(const char *s, size_t n);
void *memmem(const void *hay, size_t hlen, const void *needle, size_t nlen);

/* ---- files ----------------------------------------------------------- */
#ifndef S_ISDIR
#define S_ISDIR(m) (((m) & _S_IFMT) == _S_IFDIR)
#endif
#ifndef S_ISREG
#define S_ISREG(m) (((m) & _S_IFMT) == _S_IFREG)
#endif
#define S_ISLNK(m) 0
#define lstat stat
#define mkdir(path, mode) _mkdir(path)
/* POSIX rename: replaces `to` if it exists. */
int posix_rename(const char *from, const char *to);
#define rename(from, to) posix_rename((from), (to))
/* Path of `path` made absolute, like POSIX realpath (out: PATH_MAX bytes). */
char *realpath(const char *path, char *out);

/* ---- environment and time -------------------------------------------- */
int setenv(const char *name, const char *value, int overwrite);
int unsetenv(const char *name);
#define CLOCK_REALTIME 0
#define CLOCK_MONOTONIC 1
typedef int clockid_t;
int clock_gettime(clockid_t clock, struct timespec *ts);
struct tm *gmtime_r(const time_t *t, struct tm *out);
/* Local time by the POSIX TZ string in the environment (tz_win32.c). */
struct tm *localtime_r(const time_t *t, struct tm *out);
void posix_tzset(void);
#define tzset posix_tzset
/* strftime with %Z from the POSIX TZ string. */
size_t posix_strftime(char *out, size_t cap, const char *fmt, const struct tm *tm);
#define strftime posix_strftime
time_t timegm(struct tm *tm);

/* ---- randomness (the mock's esp_random) ------------------------------- */
uint32_t arc4random(void);
void arc4random_buf(void *buf, size_t n);

/* ---- threads ---------------------------------------------------------- */
/* Stack a thread sets aside to handle its own overflow (host.h's
   host_thread_crash_guard); pthread_create reserves it on top. */
#define HOST_CRASH_GUARD_BYTES (64 * 1024)

#ifdef __cplusplus
}
#endif
