/* ESP32 mock platform - errors, logging, time, FreeRTOS delay, heap. */
#ifndef _WIN32
#include <errno.h>
#include <poll.h>
#endif
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mock_internal.h"
#include "sim_hw.h"

/* ---- simulated time ------------------------------------------------------
 * The chip's clock is the host's monotonic clock plus a skew. Normally the
 * skew stays 0 and time runs in real time. sim_time_advance() fast-forwards:
 * the clock stands, and only the firmware moves it on. Its delays
 * (vTaskDelay) return at once and add the time they would have slept, so
 * the main loop runs tick by tick as usual, only without waiting. A
 * firmware busy without a delay (a script in a loop) adds the CPU time it
 * uses. So advance=MS is MS of the firmware's time however busy the
 * computer is: a thread the computer doesn't get to uses no CPU time, and a
 * finger put down before the advance is seen by the passes that run through
 * it. Host-side threads of the mock (mock_sleep_ms) wait for simulated time
 * without driving it. When the fast-forward is used up, the clock runs on in
 * real time from where it stood (or stands while control lines wait behind
 * it, sim_time_hold_after).
 *
 * Input on the PC link (sim_time_wake_on_input) ends a one-tick delay at
 * once, as if the tick had come right then. On the chip vTaskDelay(1) ends
 * at the next tick interrupt, anywhere up to a tick away; here it's when the
 * host sends something, so a command from the PC doesn't wait out a tick.
 * The clock doesn't move for it: a megabyte coming in over the link is
 * hundreds of passes, not seconds of the chip's time. */
#define TICK_US ((int64_t)portTICK_PERIOD_MS * 1000)

static pthread_mutex_t s_tlock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_tcond = PTHREAD_COND_INITIALIZER;   /* the clock jumped or got new fast-forward */
static int64_t s_boot_us;               /* host monotonic time at boot */
static _Atomic int64_t s_skew_us;       /* simulated minus host time (written under s_tlock) */
static int64_t s_ff_us;                 /* fast-forward left (under s_tlock) */
static atomic_bool s_ff_on;             /* s_ff_us > 0, for who doesn't hold s_tlock */
static atomic_uint s_ff_pass;           /* the firmware's delays since the fast-forward began */
static atomic_bool s_boot_set;
static host_link_t *s_wake;           /* the PC link */
static atomic_bool s_held;              /* the clock stands: in a fast-forward, or for control lines */
static _Atomic int64_t s_held_us;       /* the clock while held */
static bool s_hold_next;                /* stay held when the fast-forward ends (under s_tlock) */
static pthread_t s_fw_thread;           /* the firmware's (the first to call vTaskDelay) */
static atomic_bool s_fw_known;
static unsigned s_busy_reads;           /* firmware thread only: clock reads since its last delay */
static bool s_busy;                     /* firmware thread only: busy, its CPU time is charged */
static int64_t s_busy_cpu_us;           /* ... charged up to this */

void sim_time_init(void)
{
    pthread_mutex_lock(&s_tlock);
    s_boot_us = host_mono_us();
    s_skew_us = 0;
    s_ff_us = 0;
    atomic_store(&s_ff_on, false);
    s_hold_next = false;
    atomic_store(&s_held, false);
    atomic_store(&s_boot_set, true);
    pthread_mutex_unlock(&s_tlock);
}

/* Held: the clock stands during a fast-forward (only the firmware moves it)
   and while control lines sent together are applied (sim_time_hold_after,
   sim_time_release). On a busy machine the simulator's main thread may get
   to the next line only a while later, and a finger lifted then had been
   down that much longer (a tap became a hold). */
static int64_t clock_now_us(void) { return host_mono_us() - s_boot_us + atomic_load(&s_skew_us); }

static void hold_locked(void)
{
    if (atomic_load(&s_held)) return;
    atomic_store(&s_held_us, clock_now_us());
    atomic_store(&s_held, true);
}

static void release_locked(void);

/* The fast-forward is used up: the clock runs on, unless lines wait behind it. */
static void ff_done_locked(void)
{
    if (s_hold_next) s_hold_next = false;
    else release_locked();
}

/* Use up `us` of the fast-forward: the clock moves that much. */
static void ff_step_locked(int64_t us)
{
    s_ff_us -= us;
    atomic_store(&s_ff_on, s_ff_us > 0);
    atomic_fetch_add(&s_held_us, us);
    if (s_ff_us <= 0) ff_done_locked();
    pthread_cond_broadcast(&s_tcond);
}

static bool on_fw_thread(void) { return atomic_load(&s_fw_known) && pthread_equal(pthread_self(), s_fw_thread); }

/* The firmware reads the clock in a fast-forward. Many reads without a
   delay between them: it's busy (a script in a loop, its watchdog looking
   at the clock), and the CPU time it uses moves the clock, in steps of a
   millisecond or more. */
#define BUSY_READS 256
static void busy_charge(void)
{
    if (++s_busy_reads < BUSY_READS) return;
    int64_t cpu = host_thread_cpu_us();
    if (!s_busy) { s_busy = true; s_busy_cpu_us = cpu; return; }
    int64_t used = cpu - s_busy_cpu_us;
    if (used < 1000) return;
    s_busy_cpu_us = cpu;
    pthread_mutex_lock(&s_tlock);
    if (s_ff_us > 0) ff_step_locked(used < s_ff_us ? used : s_ff_us);
    pthread_mutex_unlock(&s_tlock);
}

int64_t esp_timer_get_time(void)
{
    if (!atomic_load(&s_boot_set)) sim_time_init();
    if (atomic_load(&s_held)) {
        if (atomic_load(&s_ff_on) && on_fw_thread()) busy_charge();
        return atomic_load(&s_held_us);
    }
    return clock_now_us();
}

static void release_locked(void)
{
    s_hold_next = false;
    if (s_ff_us > 0) return;                         /* the fast-forward keeps it: it runs on at its end */
    if (!atomic_load(&s_held)) return;
    atomic_store(&s_skew_us, atomic_load(&s_held_us) - (host_mono_us() - s_boot_us));
    atomic_store(&s_held, false);
    pthread_cond_broadcast(&s_tcond);
}

/* ---- wall clocks: the world's and the firmware's system clock ---------- */

static _Atomic int64_t s_world_base_us;   /* world time at chip time 0 */
static atomic_bool s_world_set;
static _Atomic int64_t s_system_off_us;   /* firmware's system clock minus the world's */

int64_t sim_world_us(void)
{
    if (!atomic_load(&s_world_set)) {
        atomic_store(&s_world_base_us, host_utc_us() - esp_timer_get_time());   /* this computer's */
        atomic_store(&s_world_set, true);
    }
    return atomic_load(&s_world_base_us) + esp_timer_get_time();
}

void sim_world_set(int64_t utc_us)
{
    atomic_store(&s_world_base_us, utc_us - esp_timer_get_time());
    atomic_store(&s_world_set, true);
}

void sim_system_clock_sync(void) { atomic_store(&s_system_off_us, 0); }

int sim_gettimeofday(struct timeval *tv, void *tz)
{
    (void)tz;
    int64_t t = sim_world_us() + atomic_load(&s_system_off_us);
    tv->tv_sec = (time_t)(t / 1000000);     /* (a long on Windows: until 2038) */
    tv->tv_usec = (suseconds_t)(t % 1000000);
    return 0;
}

int sim_settimeofday(const struct timeval *tv, const void *tz)
{
    (void)tz;
    atomic_store(&s_system_off_us, (int64_t)tv->tv_sec * 1000000 + tv->tv_usec - sim_world_us());
    return 0;
}

time_t sim_time(time_t *t)
{
    struct timeval tv;
    sim_gettimeofday(&tv, NULL);
    if (t) *t = tv.tv_sec;
    return tv.tv_sec;
}

/* The board is powered down (the PMIC cut the power, or the chip is in
   deep sleep): the firmware runs no more. Its main task is parked at its
   next delay, and a fast-forward moves the clock at once, as nothing is
   left to run through the time. Only a new boot ends it. */
static atomic_bool s_powered_off;

void sim_power_off(void)
{
    pthread_mutex_lock(&s_tlock);
    atomic_store(&s_powered_off, true);
    if (s_ff_us > 0) {               /* what's left of a fast-forward goes by at once */
        s_hold_next = false;
        ff_step_locked(s_ff_us);
    }
    pthread_cond_broadcast(&s_tcond);
    pthread_mutex_unlock(&s_tlock);
}
bool sim_powered_off(void) { return atomic_load(&s_powered_off); }

/* Deep sleep is a board that runs nothing too, but one the chip's own timer
   can end (esp_sleep_enable_timer_wakeup): `timer_us` from now, or never
   with a negative value. */
static atomic_bool s_deep_sleep;
static _Atomic int64_t s_deep_wake_us = -1;

void sim_deep_sleep(int64_t timer_us)
{
    atomic_store(&s_deep_wake_us, timer_us >= 0 ? esp_timer_get_time() + timer_us : -1);
    atomic_store(&s_deep_sleep, true);
    sim_power_off();
}

bool sim_deep_sleep_wake_due(void)
{
    int64_t at = atomic_load(&s_deep_wake_us);
    return atomic_load(&s_deep_sleep) && at >= 0 && esp_timer_get_time() >= at;
}

static void park_if_off(void)
{
    while (atomic_load(&s_powered_off)) host_sleep_ms(1000);
}

void sim_time_advance(uint32_t ms)
{
    if (atomic_load(&s_powered_off)) {                /* nothing runs: the clock just moves */
        sim_time_skip(ms);
        return;
    }
    pthread_mutex_lock(&s_tlock);
    hold_locked();                                   /* only the firmware moves it now */
    if (s_ff_us <= 0) atomic_store(&s_ff_pass, 0);
    s_hold_next = false;
    s_ff_us += (int64_t)ms * 1000;
    atomic_store(&s_ff_on, s_ff_us > 0);
    if (s_ff_us <= 0) ff_done_locked();              /* advance=0: nothing to run through */
    pthread_cond_broadcast(&s_tcond);
    pthread_mutex_unlock(&s_tlock);
    host_link_kick(s_wake);                          /* a delay waiting on the link goes on */
}

void sim_time_hold_after(void)
{
    pthread_mutex_lock(&s_tlock);
    if (s_ff_us > 0) s_hold_next = true;             /* held meanwhile: it stays held at the end */
    else hold_locked();
    pthread_mutex_unlock(&s_tlock);
}

void sim_time_release(void)
{
    pthread_mutex_lock(&s_tlock);
    release_locked();
    pthread_mutex_unlock(&s_tlock);
}

void sim_time_skip(uint32_t ms)
{
    pthread_mutex_lock(&s_tlock);
    if (atomic_load(&s_held)) atomic_fetch_add(&s_held_us, (int64_t)ms * 1000);
    atomic_fetch_add(&s_skew_us, (int64_t)ms * 1000);
    pthread_cond_broadcast(&s_tcond);
    pthread_mutex_unlock(&s_tlock);
}

void sim_time_wake_on_input(host_link_t *link) { s_wake = link; }

/* In a fast-forward a pass of the firmware's main loop takes well under a
   microsecond, and looking at the PC link is a system call: most of the
   pass. On macOS it's worse than slow: all pseudo-terminals share a lock in
   the kernel, and a dozen simulators fast-forwarding at once slowed every
   other simulator's link to a sixtieth (a screenshot: 14 ms alone, 900 ms
   then). So in a fast-forward the link is looked at in the first pass and
   every LINK_EVERY passes after it. What the PC sends in between is read
   that much later by the chip's clock; in real time it's microseconds. */
#define LINK_EVERY 64

bool sim_link_look(void)
{
    return !atomic_load(&s_ff_on) || atomic_load(&s_ff_pass) % LINK_EVERY == 0;
}

bool sim_time_advancing(void) { return atomic_load(&s_ff_on); }

/* Threads beside the firmware's main one (the speaker's, the name
   lookups', the mock's Wi-Fi joining) wait for a time on the chip's clock
   or for work, on a condition variable. A fast-forward keeps in step with
   them: it doesn't run past a time one of them waits for, and once that
   time has come, or the thread was given work (sim_cond_signal), it waits
   for the thread to be done: to wait for a time or for work again. So such
   a thread falls no further behind the firmware in a fast-forward than it
   does in real time. Without this the firmware ran ticks ahead each time
   the speaker's thread refilled its 23 ms buffer or was woken for a new
   sound, the sound came out stretched, and a minute of beeps was still
   playing seconds after the minute.

   A thread that does neither within HOLD_US of the host's time (it was
   woken for nothing, or is busy for long) is waited for no longer. */
#define THREADS_MAX 16
#define HOLD_US     50000
#define NEVER       INT64_MAX
static struct {
    bool used;
    int64_t at;                 /* the chip time it waits for; NEVER: only for work; 0: it's awake and waited for */
    int64_t hold_until;         /* awake: the host time it's waited for until */
    pthread_cond_t *cond;       /* waiting for work too: the condition variable */
    pthread_t thread;
} s_threads[THREADS_MAX];       /* (under s_tlock) */

/* The earliest time a thread waits for (0: one is awake and waited for;
   NEVER: none), and how long to wait for one that is awake. A thread that
   waits for work until a time `now` has reached is woken. */
static int64_t first_thread_locked(int64_t now, int64_t *wait_us)
{
    int64_t first = NEVER, host = host_mono_us();
    *wait_us = 2000;
    for (int i = 0; i < THREADS_MAX; i++) {
        if (!s_threads[i].used) continue;
        if (s_threads[i].cond && !s_threads[i].hold_until && s_threads[i].at <= now) {
            s_threads[i].at = 0;
            s_threads[i].hold_until = host + HOLD_US;
            pthread_cond_broadcast(s_threads[i].cond);
        }
        if (s_threads[i].hold_until) {
            if (host >= s_threads[i].hold_until) {                       /* long enough */
                s_threads[i].hold_until = 0;
                if (s_threads[i].cond) s_threads[i].at = NEVER;          /* (still waiting for work) */
                else s_threads[i].used = false;
                continue;
            }
            if (s_threads[i].hold_until - host < *wait_us) *wait_us = s_threads[i].hold_until - host;
        }
        if (s_threads[i].at < first) first = s_threads[i].at;
    }
    return first;
}

static int thread_slot_locked(void)
{
    int64_t host = host_mono_us();
    for (int i = 0; i < THREADS_MAX; i++) {
        if (s_threads[i].used && !s_threads[i].cond && s_threads[i].hold_until && host >= s_threads[i].hold_until)
            s_threads[i].used = false;                   /* (one that was waited for long enough) */
        if (!s_threads[i].used) return i;
    }
    return -1;
}

/* This thread is done with what it woke up for: a fast-forward goes on. */
static void done_locked(void)
{
    pthread_t self = pthread_self();
    for (int i = 0; i < THREADS_MAX; i++) {
        if (s_threads[i].used && !s_threads[i].cond && s_threads[i].hold_until && pthread_equal(s_threads[i].thread, self)) {
            s_threads[i].used = false;
            pthread_cond_broadcast(&s_tcond);
        }
    }
}

void sim_time_idle(void)
{
    pthread_mutex_lock(&s_tlock);
    done_locked();
    pthread_mutex_unlock(&s_tlock);
}

/* Wait for work on `cond`, and at most until the chip's clock reads `at_us`
   (NEVER: for work only). The clock can jump, so with a time it's looked at
   again every 2 ms of the host's time at least: this can return early. */
int sim_cond_wait_until(pthread_cond_t *cond, pthread_mutex_t *mutex, int64_t at_us)
{
    pthread_mutex_lock(&s_tlock);
    done_locked();
    int slot = thread_slot_locked();
    if (slot >= 0) {
        s_threads[slot].used = true;
        s_threads[slot].at = at_us;
        s_threads[slot].hold_until = 0;
        s_threads[slot].cond = cond;
        s_threads[slot].thread = pthread_self();
    }
    pthread_mutex_unlock(&s_tlock);
    int rc;
    if (at_us == NEVER) {
        rc = pthread_cond_wait(cond, mutex);
    } else {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 2 * 1000000L;
        if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        rc = pthread_cond_timedwait(cond, mutex, &ts);
    }
    if (slot >= 0) {
        pthread_mutex_lock(&s_tlock);
        s_threads[slot].cond = NULL;                     /* awake: waited for if it was given work, until it's done */
        if (!s_threads[slot].hold_until) s_threads[slot].used = false;
        pthread_cond_broadcast(&s_tcond);
        pthread_mutex_unlock(&s_tlock);
    }
    return rc;
}

int sim_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex) { return sim_cond_wait_until(cond, mutex, NEVER); }

/* The threads waiting on `cond` (one, or all) are given work: a
   fast-forward, also one that starts right after, waits for them. */
static void given_work(pthread_cond_t *cond, bool all)
{
    pthread_mutex_lock(&s_tlock);
    for (int i = 0; i < THREADS_MAX; i++) {
        if (!s_threads[i].used || s_threads[i].cond != cond || s_threads[i].hold_until) continue;
        s_threads[i].at = 0;
        s_threads[i].hold_until = host_mono_us() + HOLD_US;
        if (!all) break;
    }
    pthread_mutex_unlock(&s_tlock);
}

/* A thread the firmware starts (its name lookups' worker, at the first
   lookup) begins with work to do, like one that was given some: a
   fast-forward that starts right after waits until it waits for the first
   time. Without this the worker of the first lookup began its 3 s for a
   name nobody knows after an advance of 3 s had gone by, where the two
   threads didn't get to run at the same time (Linux, two processors). */
int sim_pthread_create(pthread_t *thread, const pthread_attr_t *attr, void *(*fn)(void *), void *arg)
{
    pthread_mutex_lock(&s_tlock);                        /* (held: the new thread's first wait finds its slot) */
    int rc = pthread_create(thread, attr, fn, arg);
    int slot = rc == 0 ? thread_slot_locked() : -1;
    if (slot >= 0) {
        s_threads[slot].used = true;
        s_threads[slot].at = 0;
        s_threads[slot].hold_until = host_mono_us() + HOLD_US;
        s_threads[slot].cond = NULL;
        s_threads[slot].thread = *thread;
    }
    pthread_mutex_unlock(&s_tlock);
    return rc;
}

int sim_cond_signal(pthread_cond_t *cond)
{
    given_work(cond, false);
    return pthread_cond_signal(cond);
}

int sim_cond_broadcast(pthread_cond_t *cond)
{
    given_work(cond, true);
    return pthread_cond_broadcast(cond);
}

/* The firmware's connections to servers. A server answers in real time, in
   well under a millisecond here, which is nothing to the chip. A
   fast-forward would run seconds of the chip's time past that (into the
   firmware's timeout for the answer), so once the firmware has sent
   something on a connection it made, a fast-forward waits until something
   has come in on it. A server that stays silent is waited for HOLD_US of
   the host's time, once: not again until it has said something. */
#define NET_MAX 16
static struct {
    int fd;
    enum { N_FREE, N_QUIET, N_ASKED, N_SILENT } state;   /* asked: an answer is waited for (until hold_until) */
    int64_t hold_until;
} s_net[NET_MAX];                                        /* (under s_tlock) */

static int net_slot_locked(int fd, bool add)
{
    int free_slot = -1;
    for (int i = 0; i < NET_MAX; i++) {
        if (s_net[i].state != N_FREE && s_net[i].fd == fd) return i;
        if (s_net[i].state == N_FREE && free_slot < 0) free_slot = i;
    }
    if (add && free_slot >= 0) s_net[free_slot].fd = fd;
    return add ? free_slot : -1;
}

void sim_net_client(int fd)
{
    pthread_mutex_lock(&s_tlock);
    int i = net_slot_locked(fd, true);
    if (i >= 0) s_net[i].state = N_QUIET;
    pthread_mutex_unlock(&s_tlock);
}

void sim_net_sent(int fd)
{
    pthread_mutex_lock(&s_tlock);
    int i = net_slot_locked(fd, false);
    if (i >= 0 && s_net[i].state == N_QUIET) {
        s_net[i].state = N_ASKED;
        s_net[i].hold_until = host_mono_us() + HOLD_US;
    }
    pthread_mutex_unlock(&s_tlock);
}

/* `fd` is no connection the firmware made (any more): a socket it accepted
   or bound got that number after the one it made was closed. */
void sim_net_forget(int fd)
{
    pthread_mutex_lock(&s_tlock);
    int i = net_slot_locked(fd, false);
    if (i >= 0) s_net[i].state = N_FREE;
    pthread_mutex_unlock(&s_tlock);
}

void sim_net_received(int fd)
{
    pthread_mutex_lock(&s_tlock);
    int i = net_slot_locked(fd, false);
    if (i >= 0) s_net[i].state = N_QUIET;
    pthread_mutex_unlock(&s_tlock);
}

/* In a fast-forward: is an answer still on its way? Waits up to 1 ms of the
   host's time for one (s_tlock let go meanwhile). */
static bool net_wait_locked(void)
{
#ifdef _WIN32
    return false;
#else
    struct pollfd fds[NET_MAX];
    int slot[NET_MAX], n = 0;
    int64_t host = host_mono_us();
    for (int i = 0; i < NET_MAX; i++) {
        if (s_net[i].state != N_ASKED) continue;
        if (host >= s_net[i].hold_until) { s_net[i].state = N_SILENT; continue; }
        fds[n] = (struct pollfd){ .fd = s_net[i].fd, .events = POLLIN };
        slot[n++] = i;
    }
    if (!n) return false;
    pthread_mutex_unlock(&s_tlock);
    int got = poll(fds, (nfds_t)n, 1);
    pthread_mutex_lock(&s_tlock);
    bool waiting = false;
    for (int k = 0; k < n; k++) {
        int i = slot[k];
        if (s_net[i].state != N_ASKED || s_net[i].fd != fds[k].fd) continue;
        if (got > 0 && (fds[k].revents & POLLNVAL)) s_net[i].state = N_FREE;          /* closed */
        else if (got > 0 && fds[k].revents) s_net[i].state = N_QUIET;                 /* it's there for the firmware */
        else waiting = true;
    }
    return waiting;
#endif
}

/* Wait on s_tcond for at most `us` of the host's time. */
static void tcond_wait_locked(int64_t us)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);              /* pthread_cond_timedwait's clock */
    int64_t ns = ts.tv_nsec + (us % 1000000) * 1000;
    ts.tv_sec += (time_t)(us / 1000000 + ns / 1000000000);
    ts.tv_nsec = (long)(ns % 1000000000);
    pthread_cond_timedwait(&s_tcond, &s_tlock, &ts);
}

/* Block until the simulated clock reads `deadline` (µs since boot). `drive`:
   take the time from the fast-forward first (the firmware's delays). */
static void sleep_until(int64_t deadline, bool drive)
{
    pthread_mutex_lock(&s_tlock);
    done_locked();
    int slot = -1;
    if (!drive && deadline > esp_timer_get_time()) {     /* (no free slot: woken at every step, and not waited for) */
        slot = thread_slot_locked();
        if (slot >= 0) {
            s_threads[slot].used = true;
            s_threads[slot].at = deadline;
            s_threads[slot].hold_until = 0;
            s_threads[slot].cond = NULL;
            s_threads[slot].thread = pthread_self();
        }
    }
    for (;;) {
        int64_t now = esp_timer_get_time(), left = deadline - now;
        if (left <= 0) break;
        if (drive && s_ff_us > 0) {
            int64_t wait_us, first = first_thread_locked(now, &wait_us);
            if (first <= now) {                          /* a thread's turn */
                tcond_wait_locked(wait_us);
                continue;
            }
            if (net_wait_locked()) continue;             /* a server's answer goes first too */
            int64_t step = left < s_ff_us ? left : s_ff_us;
            if (first - now < step) step = first - now;
            ff_step_locked(step);
            continue;
        }
        tcond_wait_locked(atomic_load(&s_held) ? 50000 : left);   /* held: until it moves (a broadcast) */
    }
    if (slot >= 0) {                                     /* its time has come: a fast-forward waits for it */
        s_threads[slot].at = 0;
        s_threads[slot].hold_until = host_mono_us() + HOLD_US;
        pthread_cond_broadcast(&s_tcond);
    }
    pthread_mutex_unlock(&s_tlock);
}

void mock_sleep_ms(uint32_t ms)
{
    sleep_until(esp_timer_get_time() + (int64_t)ms * 1000, false);
}

void mock_sleep_until_us(int64_t deadline_us) { sleep_until(deadline_us, false); }

/* The sockets the firmware read from since its last delay, with or without
   finding something (sim_net_waits). Input on one of them ends a one-tick delay like input
   on the PC link does: the firmware reads its sockets once a pass, and a
   megabyte coming in was a pass every 10 ms, seconds of real time. */
#define WAITS_MAX 16
static int s_waits[WAITS_MAX];
static int s_nwaits;                                     /* (under s_tlock) */

void sim_net_waits(int fd)
{
    int e = errno;                                       /* (the caller's EAGAIN stays) */
    pthread_mutex_lock(&s_tlock);
    int i = 0;
    while (i < s_nwaits && s_waits[i] != fd) i++;
    if (i == s_nwaits && s_nwaits < WAITS_MAX) s_waits[s_nwaits++] = fd;
    pthread_mutex_unlock(&s_tlock);
    errno = e;
}

static int waits_take(int *fds)
{
    pthread_mutex_lock(&s_tlock);
    int n = s_nwaits;
    for (int i = 0; i < n; i++) fds[i] = s_waits[i];
    s_nwaits = 0;
    pthread_mutex_unlock(&s_tlock);
    return n;
}

/* Wait one tick at most; true if input (on the PC link, or on one of
   `socks`) ended it early. */
static bool input_within_tick(const int *socks, int n)
{
    return host_link_wait(s_wake, portTICK_PERIOD_MS, socks, n);
}

static void (*s_delay_hook)(void);
void sim_set_delay_hook(void (*fn)(void)) { s_delay_hook = fn; }

void vTaskDelay(TickType_t ticks)
{
    if (s_delay_hook) s_delay_hook();
    park_if_off();
    if (mock_ota_fail_boot() && esp_timer_get_time() >= 1000000) {
        fprintf(stderr, "simulated crash: this image (fail=boot) crashes 1 s into every boot\n");
        mock_restart(ESP_RST_PANIC);
    }
    if (!atomic_load(&s_fw_known)) {
        s_fw_thread = pthread_self();
        atomic_store(&s_fw_known, true);
    }
    s_busy_reads = 0;                                    /* a delay: not busy */
    s_busy = false;
    int64_t deadline = esp_timer_get_time() + (int64_t)ticks * TICK_US;
    atomic_fetch_add(&s_ff_pass, 1);
    int socks[WAITS_MAX], nsocks = waits_take(socks);
    if (ticks == 1 && s_wake && !sim_time_advancing() && input_within_tick(socks, nsocks)) return;   /* the tick ends now */
    sleep_until(deadline, true);
}

TickType_t xTaskGetTickCount(void)
{
    return (TickType_t)(esp_timer_get_time() / TICK_US);
}

static esp_reset_reason_t s_reset_reason = ESP_RST_POWERON;
void sim_set_reset_reason(esp_reset_reason_t r) { s_reset_reason = r; }
esp_reset_reason_t esp_reset_reason(void) { return s_reset_reason; }

static void (*s_restart)(esp_reset_reason_t);
void sim_set_restart_handler(void (*restart)(esp_reset_reason_t reason)) { s_restart = restart; }

void mock_restart(esp_reset_reason_t reason)
{
    if (s_restart) s_restart(reason);
    fprintf(stderr, "esp_restart: no restart handler, stopping\n");
    _exit(1);
}

void esp_restart(void) { mock_restart(ESP_RST_SW); }

uint32_t esp_get_free_heap_size(void)
{
    return 280 * 1024; /* typical free heap on a fresh ESP32 app */
}

const char *esp_err_to_name(esp_err_t code)
{
    switch (code) {
    case ESP_OK: return "ESP_OK";
    case ESP_FAIL: return "ESP_FAIL";
    case ESP_ERR_NO_MEM: return "ESP_ERR_NO_MEM";
    case ESP_ERR_INVALID_ARG: return "ESP_ERR_INVALID_ARG";
    case ESP_ERR_INVALID_STATE: return "ESP_ERR_INVALID_STATE";
    case ESP_ERR_INVALID_SIZE: return "ESP_ERR_INVALID_SIZE";
    case ESP_ERR_NOT_FOUND: return "ESP_ERR_NOT_FOUND";
    case ESP_ERR_NOT_SUPPORTED: return "ESP_ERR_NOT_SUPPORTED";
    case ESP_ERR_TIMEOUT: return "ESP_ERR_TIMEOUT";
    case ESP_ERR_NVS_NOT_FOUND: return "ESP_ERR_NVS_NOT_FOUND";
    case ESP_ERR_NVS_NOT_ENOUGH_SPACE: return "ESP_ERR_NVS_NOT_ENOUGH_SPACE";
    case ESP_ERR_NVS_INVALID_LENGTH: return "ESP_ERR_NVS_INVALID_LENGTH";
    case 0x1501: return "ESP_ERR_OTA_PARTITION_CONFLICT";
    case 0x1503: return "ESP_ERR_OTA_VALIDATE_FAILED";
    case 0x1506: return "ESP_ERR_OTA_ROLLBACK_INVALID_STATE";
    default: return "ESP_ERR_UNKNOWN";
    }
}

void esp_mock_log(char level, const char *tag, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "%c (%lld) %s: ", level, (long long)(esp_timer_get_time() / 1000), tag);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

/* ---- heap caps: everything is host malloc ---- */
#include <stdlib.h>
#include "esp_heap_caps.h"

void *heap_caps_malloc(size_t size, uint32_t caps) { (void)caps; return malloc(size); }
void *heap_caps_calloc(size_t n, size_t size, uint32_t caps) { (void)caps; return calloc(n, size); }
void *heap_caps_realloc(void *ptr, size_t size, uint32_t caps) { (void)caps; return realloc(ptr, size); }
void heap_caps_free(void *ptr) { free(ptr); }

/* ---- random: host CSPRNG stands in for the hardware RNG ---- */
#include "esp_random.h"

#if defined(__linux__)
/* glibc has arc4random only from 2.36; the release's Linux build is older. */
#include <sys/random.h>

void esp_fill_random(void *buf, size_t len)
{
    uint8_t *p = buf;
    while (len > 0) {
        ssize_t n = getrandom(p, len, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            abort();
        }
        p += n;
        len -= (size_t)n;
    }
}

uint32_t esp_random(void)
{
    uint32_t v;
    esp_fill_random(&v, sizeof v);
    return v;
}
#else
uint32_t esp_random(void) { return arc4random(); }
void esp_fill_random(void *buf, size_t len) { arc4random_buf(buf, len); }
#endif
