/*
 * The wake-up time of sys.off(seconds): how the clock chip (BM8563) is set
 * so the device is on again by then. Pure: no hardware (hw.c arms the chip
 * from the plan).
 *
 * The chip has a countdown timer of 1..255 counts, of a second or a minute
 * each, and an alarm that matches on the minute. Whichever is used, the
 * device comes on early, never late: the time is rounded down, after
 * taking the boot's seconds off.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define OFF_BOOT_ALLOWANCE_S 5                /* from power-on until the app runs again */
#define OFF_MIN_S            10               /* the shortest wait worth turning off for */
/* The alarm matches on the day of the month, which comes round again after
   28 days in February: a longer wait could fire weeks early, and exactly
   28 days would match the minute the device turns off in. */
#define OFF_MAX_S            (27u * 86400u)

typedef enum { WAKE_TIMER_S, WAKE_TIMER_MIN, WAKE_ALARM } wake_kind_t;

typedef struct {
    wake_kind_t kind;
    uint8_t  count;       /* WAKE_TIMER_*: the chip timer's count, 1..255 */
    int64_t  alarm_utc;   /* WAKE_ALARM: when it fires, on a whole minute */
    uint32_t after_s;     /* seconds from now until it fires */
} wake_plan_t;

typedef enum { WAKE_OK, WAKE_TOO_SHORT, WAKE_TOO_LONG, WAKE_NEEDS_CLOCK } wake_err_t;

/** Plan a wake-up `seconds` from now. The alarm (waits over 255 minutes)
 *  needs the time: WAKE_NEEDS_CLOCK unless clock_known, with now_utc the
 *  time now (UTC seconds). */
wake_err_t wake_plan(uint32_t seconds, bool clock_known, int64_t now_utc, wake_plan_t *out);
