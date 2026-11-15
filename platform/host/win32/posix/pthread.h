/*
 * <pthread.h> for MSVC: the part the firmware and the mock use, on Win32
 * slim reader/writer locks and condition variables (pthread_win32.c). Both
 * initialise statically to zero, as PTHREAD_*_INITIALIZER needs. Mutexes are
 * not recursive (nothing here asks for that).
 */
#pragma once
#include "posix_compat.h"

typedef DWORD pthread_t;                 /* the thread id */
typedef struct { size_t stack_size; } pthread_attr_t;
typedef SRWLOCK pthread_mutex_t;
typedef CONDITION_VARIABLE pthread_cond_t;
typedef int pthread_mutexattr_t;
typedef int pthread_condattr_t;

#define PTHREAD_MUTEX_INITIALIZER SRWLOCK_INIT
#define PTHREAD_COND_INITIALIZER CONDITION_VARIABLE_INIT

int pthread_create(pthread_t *t, const pthread_attr_t *attr, void *(*fn)(void *), void *arg);
int pthread_detach(pthread_t t);
pthread_t pthread_self(void);
int pthread_equal(pthread_t a, pthread_t b);
int pthread_attr_init(pthread_attr_t *a);
int pthread_attr_destroy(pthread_attr_t *a);
int pthread_attr_setstacksize(pthread_attr_t *a, size_t size);

int pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *attr);
int pthread_mutex_destroy(pthread_mutex_t *m);
int pthread_mutex_lock(pthread_mutex_t *m);
int pthread_mutex_trylock(pthread_mutex_t *m);
int pthread_mutex_unlock(pthread_mutex_t *m);

int pthread_cond_init(pthread_cond_t *c, const pthread_condattr_t *attr);
int pthread_cond_destroy(pthread_cond_t *c);
int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m);
/* `abstime` on CLOCK_REALTIME, as in POSIX. */
int pthread_cond_timedwait(pthread_cond_t *c, pthread_mutex_t *m, const struct timespec *abstime);
int pthread_cond_signal(pthread_cond_t *c);
int pthread_cond_broadcast(pthread_cond_t *c);
