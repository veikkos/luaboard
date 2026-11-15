#include "crash.h"

#include <stddef.h>
#include <string.h>

#include "esp_attr.h"
#include "hw.h"

#define CRASH_MAGIC 0x43525348u                  /* "CRSH" */

RTC_NOINIT_ATTR crash_record_t crash_rec;
static crash_record_t s_last;

/* CRC-32 by hand: in a panic nothing else is called that needn't be. */
static uint32_t crc_of(const crash_record_t *r)
{
    const uint8_t *p = (const uint8_t *)r + offsetof(crash_record_t, cause);
    size_t n = sizeof *r - offsetof(crash_record_t, cause);
    uint32_t c = 0xFFFFFFFFu;
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return ~c;
}

void crash_seal(crash_record_t *r)
{
    r->cause[sizeof r->cause - 1] = 0;
    r->version[sizeof r->version - 1] = 0;
    if (r->frames > CRASH_FRAMES) r->frames = CRASH_FRAMES;
    for (int i = 0; i < CRASH_FRAMES; i++) r->frame[i][CRASH_FRAME_LEN - 1] = 0;
    r->crc = crc_of(r);
    r->magic = CRASH_MAGIC;
}

const crash_record_t *crash_last(void)
{
    static bool taken, have;
    if (!taken) {
        taken = true;
        have = hw_reset_was_fw_crash() && crash_rec.magic == CRASH_MAGIC && crash_rec.crc == crc_of(&crash_rec);
        if (have) s_last = crash_rec;
        crash_rec.magic = 0;                     /* each crash is told once */
    }
    return have ? &s_last : NULL;
}
