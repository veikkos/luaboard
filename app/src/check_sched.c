/* Background check timing. See check_sched.h. */
#include "check_sched.h"

#include "app_config.h"

bool check_sched_due(check_sched_t *s, uint32_t now, uint32_t first_ms)
{
    if (!s->due_set) {
        s->due_set = true;
        s->due = now + first_ms;
        return false;
    }
    return (int32_t)(now - s->due) >= 0;
}

void check_sched_done(check_sched_t *s, uint32_t now, bool ok, uint32_t every_ms)
{
    uint32_t wait = every_ms;
    if (ok) {
        s->fails = 0;
    } else {
        if (s->fails == 0) wait /= UPDATE_RETRY_1_PART;
        else if (s->fails == 1) wait /= UPDATE_RETRY_2_PART;
        if (s->fails < 2) s->fails++;
    }
    s->due_set = true;
    s->due = now + wait;
}

void check_sched_reset(check_sched_t *s)
{
    *s = (check_sched_t){ 0 };
}

void check_sched_soon(check_sched_t *s, uint32_t now, uint32_t in_ms)
{
    s->due_set = true;
    s->due = now + in_ms;
}
