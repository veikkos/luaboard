#include "wake_time.h"

wake_err_t wake_plan(uint32_t seconds, bool clock_known, int64_t now_utc, wake_plan_t *out)
{
    if (seconds < OFF_MIN_S) return WAKE_TOO_SHORT;
    if (seconds > OFF_MAX_S) return WAKE_TOO_LONG;
    uint32_t left = seconds - OFF_BOOT_ALLOWANCE_S;
    *out = (wake_plan_t){ 0 };
    if (left <= 255) {
        out->kind = WAKE_TIMER_S;
        out->count = (uint8_t)left;
        out->after_s = left;
    } else if (left <= 255 * 60) {
        out->kind = WAKE_TIMER_MIN;
        out->count = (uint8_t)(left / 60);
        out->after_s = (uint32_t)out->count * 60;
    } else {
        if (!clock_known) return WAKE_NEEDS_CLOCK;
        int64_t at = now_utc + left;
        out->kind = WAKE_ALARM;
        out->alarm_utc = at - at % 60;
        out->after_s = (uint32_t)(out->alarm_utc - now_utc);
    }
    return WAKE_OK;
}
