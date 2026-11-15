/*
 * Force-included into the firmware's sources in the simulator (CMakeLists.txt):
 * the firmware's system clock runs on the simulated world clock instead of
 * the computer's, so tests can pin the date (--set time=EPOCH) and `advance`
 * moves it along with the chip's clock. See sim_hw.h.
 *
 * The firmware's threads wait for work on condition variables: a
 * fast-forward waits for a thread that was given work until it waits again,
 * so it keeps in step with the main loop (mock_core.c).
 */
#pragma once

#include <pthread.h>
#include <sys/time.h>
#include <time.h>

int sim_gettimeofday(struct timeval *tv, void *tz);
int sim_settimeofday(const struct timeval *tv, const void *tz);
time_t sim_time(time_t *t);

#define gettimeofday(tv, tz) sim_gettimeofday((tv), (tz))
#define settimeofday(tv, tz) sim_settimeofday((tv), (tz))
#define time(t) sim_time(t)

int sim_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex);
int sim_cond_signal(pthread_cond_t *cond);
int sim_cond_broadcast(pthread_cond_t *cond);
#define pthread_cond_wait(cond, mutex) sim_cond_wait((cond), (mutex))
#define pthread_cond_signal(cond) sim_cond_signal(cond)
#define pthread_cond_broadcast(cond) sim_cond_broadcast(cond)

/* A thread the firmware starts has work from the start: a fast-forward
   waits for it too, until it first waits. */
int sim_pthread_create(pthread_t *thread, const pthread_attr_t *attr, void *(*fn)(void *), void *arg);
#define pthread_create(thread, attr, fn, arg) sim_pthread_create((thread), (attr), (fn), (arg))
