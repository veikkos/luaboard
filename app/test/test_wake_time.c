#include "check.h"
#include "wake_time.h"

void test_wake_time(int *total, int *failed);

/* The wake-up time of sys.off(seconds): which of the clock chip's timers
   it takes, and that the device is always on early, never late. */
void test_wake_time(int *total, int *failed)
{
    const int64_t now = 600030;       /* 30 s past a whole minute */
    wake_plan_t p;

    CHECK(wake_plan(9, true, now, &p) == WAKE_TOO_SHORT);
    CHECK(wake_plan(0, true, now, &p) == WAKE_TOO_SHORT);
    CHECK(wake_plan(10, true, now, &p) == WAKE_OK && p.kind == WAKE_TIMER_S && p.count == 5 && p.after_s == 5);
    CHECK(wake_plan(260, true, now, &p) == WAKE_OK && p.kind == WAKE_TIMER_S && p.count == 255 && p.after_s == 255);
    CHECK(wake_plan(261, true, now, &p) == WAKE_OK && p.kind == WAKE_TIMER_MIN && p.count == 4 && p.after_s == 240);
    CHECK(wake_plan(3600, false, now, &p) == WAKE_OK && p.kind == WAKE_TIMER_MIN && p.count == 59 && p.after_s == 3540);
    CHECK(wake_plan(15305, true, now, &p) == WAKE_OK && p.kind == WAKE_TIMER_MIN && p.count == 255);
    CHECK(wake_plan(15306, true, now, &p) == WAKE_OK && p.kind == WAKE_ALARM && p.alarm_utc == 615300 &&
          p.after_s == 15270);
    CHECK(wake_plan(15306, false, now, &p) == WAKE_NEEDS_CLOCK);
    CHECK(wake_plan(OFF_MAX_S, true, now, &p) == WAKE_OK && p.kind == WAKE_ALARM);
    CHECK(wake_plan(OFF_MAX_S + 1, true, now, &p) == WAKE_TOO_LONG);

    /* early, never late, and never more than a minute early */
    for (uint32_t s = OFF_MIN_S; s < 40000; s += 7) {
        CHECK(wake_plan(s, true, now, &p) == WAKE_OK);
        CHECK(p.after_s + OFF_BOOT_ALLOWANCE_S <= s && s - p.after_s - OFF_BOOT_ALLOWANCE_S < 60);
    }

    *total += check_total;
    *failed += check_failed;
}
