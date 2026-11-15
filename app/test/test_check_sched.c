#include "check.h"
#include "check_sched.h"

void test_check_sched(int *total, int *failed);

/* When the update checks run (firmware releases and marketplace apps): a
   minute after Wi-Fi first comes up, then once a day; a failed check is
   tried again sooner, a 144th of the day, then a 24th, then a day. */
void test_check_sched(int *total, int *failed)
{
    const uint32_t FIRST = 60000, DAY = 144000;
    check_sched_t s = { 0 };
    CHECK(!check_sched_due(&s, 1000, FIRST));                /* Wi-Fi just came up */
    CHECK(!check_sched_due(&s, 60999, FIRST));
    CHECK(check_sched_due(&s, 61000, FIRST));
    check_sched_done(&s, 62000, true, DAY);
    CHECK(!check_sched_due(&s, 62000 + DAY - 1, FIRST));
    CHECK(check_sched_due(&s, 62000 + DAY, FIRST));
    check_sched_done(&s, 300000, false, DAY);                 /* failed: a 144th of the day */
    CHECK(check_sched_due(&s, 300000 + DAY / 144, FIRST) && !check_sched_due(&s, 300000 + DAY / 144 - 1, FIRST));
    check_sched_done(&s, 400000, false, DAY);                 /* again: a 24th */
    CHECK(check_sched_due(&s, 400000 + DAY / 24, FIRST) && !check_sched_due(&s, 400000 + DAY / 24 - 1, FIRST));
    check_sched_done(&s, 500000, false, DAY);                 /* and again: the whole day */
    CHECK(check_sched_due(&s, 500000 + DAY, FIRST) && !check_sched_due(&s, 500000 + DAY - 1, FIRST));
    check_sched_done(&s, 700000, true, DAY);                  /* one that works starts over */
    check_sched_done(&s, 800000, false, DAY);
    CHECK(check_sched_due(&s, 800000 + DAY / 144, FIRST));

    check_sched_reset(&s);                                    /* the setting went off */
    CHECK(!check_sched_due(&s, 900000, FIRST) && check_sched_due(&s, 960000, FIRST));
    check_sched_soon(&s, 1000000, 60000);                     /* and on again: a minute */
    CHECK(!check_sched_due(&s, 1059999, FIRST) && check_sched_due(&s, 1060000, FIRST));

    *total += check_total;
    *failed += check_failed;
}
