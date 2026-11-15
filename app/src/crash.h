/*
 * What a crash leaves for the next boot: its cause and a stack trace, the
 * firmware version that crashed, kept in RTC memory (RTC_NOINIT_ATTR) that
 * survives the reset. On the chip ESP-IDF's panic handler fills it
 * (app/idf/main/panic_hook.c); in the simulator its crash handler does, and
 * keeps it through the restart in a file, as the chip keeps RTC memory.
 *
 * Frames are text: "0x4200a1b4" on the chip (the PC tools turn those into
 * function and line with the build's ELF), function names in the simulator.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define CRASH_FRAMES    12
#define CRASH_FRAME_LEN 40

typedef struct {
    uint32_t magic;                              /* CRASH_MAGIC once sealed */
    uint32_t crc;                                /* of everything after it */
    char cause[48];                              /* "LoadProhibited, data 0x00000000", "SIGSEGV" */
    char version[32];                            /* the firmware that crashed */
    uint32_t frames;
    char frame[CRASH_FRAMES][CRASH_FRAME_LEN];   /* the innermost first */
} crash_record_t;

/** The record itself: written while crashing, read at the next boot. */
extern crash_record_t crash_rec;

/** Mark crash_rec as holding a crash (after filling it). Safe in a panic. */
void crash_seal(crash_record_t *r);

/** The last crash's record, taken at boot when the reset was a crash; NULL
 *  if there is none (or it didn't survive). Clears crash_rec. */
const crash_record_t *crash_last(void);
