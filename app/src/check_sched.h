/* When a background check runs (the firmware release check in update.c,
   the marketplace check in app_update.c): first_ms after Wi-Fi first comes
   up, then every every_ms; after a failure sooner, a UPDATE_RETRY_1_PART of
   that, then a UPDATE_RETRY_2_PART, then the full time again. Pure logic:
   the caller passes the time (hw_millis). */
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool due_set;                   /* the first check's time is set (Wi-Fi has been up) */
    uint32_t due;                   /* when the next check runs */
    int fails;                      /* checks failed in a row, 2 at most */
} check_sched_t;

/** Whether a check should start now. Call only while one could run (Wi-Fi
 *  up, the setting on): the first call sets the first check first_ms away. */
bool check_sched_due(check_sched_t *s, uint32_t now, uint32_t first_ms);

/** A check is over: when the next one runs. */
void check_sched_done(check_sched_t *s, uint32_t now, bool ok, uint32_t every_ms);

/** Back to the start: the next check is first_ms after Wi-Fi is next seen up. */
void check_sched_reset(check_sched_t *s);

/** The next check in_ms from now. */
void check_sched_soon(check_sched_t *s, uint32_t now, uint32_t in_ms);
