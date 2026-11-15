/* pthreads on Win32 (posix/pthread.h): threads, SRW locks, condition variables. */
#include <pthread.h>

#include <process.h>

typedef struct {
    void *(*fn)(void *);
    void *arg;
} start_t;

static unsigned __stdcall thread_main(void *p)
{
    start_t s = *(start_t *)p;
    free(p);
    s.fn(s.arg);
    return 0;
}

int pthread_create(pthread_t *t, const pthread_attr_t *attr, void *(*fn)(void *), void *arg)
{
    start_t *s = malloc(sizeof *s);
    if (!s) return ENOMEM;
    s->fn = fn;
    s->arg = arg;
    unsigned id = 0;
    /* The stack size is what the thread may use (POSIX), not what is
       committed up front. Reserved on top: the room a thread may set aside
       to handle its own stack overflow (host_thread_crash_guard). */
    size_t stack = attr ? attr->stack_size : 0;
    if (stack) stack += HOST_CRASH_GUARD_BYTES;
    uintptr_t h = _beginthreadex(NULL, (unsigned)stack, thread_main, s, stack ? STACK_SIZE_PARAM_IS_A_RESERVATION : 0, &id);
    if (!h) { free(s); return EAGAIN; }
    /* Nothing joins a thread here: they are all detached, the handle isn't needed. */
    CloseHandle((HANDLE)h);
    if (t) *t = id;
    return 0;
}

int pthread_detach(pthread_t t) { (void)t; return 0; }
pthread_t pthread_self(void) { return GetCurrentThreadId(); }
int pthread_equal(pthread_t a, pthread_t b) { return a == b; }

int pthread_attr_init(pthread_attr_t *a) { a->stack_size = 0; return 0; }
int pthread_attr_destroy(pthread_attr_t *a) { (void)a; return 0; }

int pthread_attr_setstacksize(pthread_attr_t *a, size_t size)
{
    if (size < 16384) return EINVAL;
    a->stack_size = size;
    return 0;
}

int pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *attr)
{
    (void)attr;
    InitializeSRWLock(m);
    return 0;
}

int pthread_mutex_destroy(pthread_mutex_t *m) { (void)m; return 0; }
int pthread_mutex_lock(pthread_mutex_t *m) { AcquireSRWLockExclusive(m); return 0; }
int pthread_mutex_trylock(pthread_mutex_t *m) { return TryAcquireSRWLockExclusive(m) ? 0 : EBUSY; }
int pthread_mutex_unlock(pthread_mutex_t *m) { ReleaseSRWLockExclusive(m); return 0; }

int pthread_cond_init(pthread_cond_t *c, const pthread_condattr_t *attr)
{
    (void)attr;
    InitializeConditionVariable(c);
    return 0;
}

int pthread_cond_destroy(pthread_cond_t *c) { (void)c; return 0; }

int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m)
{
    return SleepConditionVariableSRW(c, m, INFINITE, 0) ? 0 : EINVAL;
}

int pthread_cond_timedwait(pthread_cond_t *c, pthread_mutex_t *m, const struct timespec *abstime)
{
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    long long ns = ((long long)abstime->tv_sec - now.tv_sec) * 1000000000LL + (abstime->tv_nsec - now.tv_nsec);
    long long ms = ns <= 0 ? 0 : (ns + 999999) / 1000000;   /* rounded up: never wake before the time */
    if (ms > 0x7FFFFFFF) ms = 0x7FFFFFFF;
    if (SleepConditionVariableSRW(c, m, (DWORD)ms, 0)) return 0;
    return GetLastError() == ERROR_TIMEOUT ? ETIMEDOUT : EINVAL;
}

int pthread_cond_signal(pthread_cond_t *c) { WakeConditionVariable(c); return 0; }
int pthread_cond_broadcast(pthread_cond_t *c) { WakeAllConditionVariable(c); return 0; }
