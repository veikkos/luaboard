/*
 * The microSD card: a FAT file system mounted at SD_MOUNT while a script
 * uses it (lua_sd.c reaches the files with stdio).
 *
 * On the CoreS3 the card sits on the display's SPI bus, and its data-out
 * line is the pin the display uses as D/C. So the pin changes hands around
 * every use of the card: sdcard_begin() makes it an input for the card to
 * answer on, sdcard_end() gives it back to the display. Nothing may draw
 * in between, and no Lua error may leave the two unpaired.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

void sdcard_begin(void);
void sdcard_end(void);

/* All of these between sdcard_begin() and sdcard_end(). */
/** Mount the card if it isn't. NULL when it's there, else why not ("no
 *  card", not FAT). An empty slot isn't tried again for SD_RETRY_MS. */
const char *sdcard_mount(void);
/** Before a file operation, and after a failed one: if the card has gone
 *  (or another is in its place), let go of it, so the call looks for a
 *  card again. */
void sdcard_check(void);
/** Let go of the card (the script ended). */
void sdcard_unmount(void);
bool sdcard_info(uint64_t *total, uint64_t *free_bytes);
/** Where the card's files are while it's mounted (SD_MOUNT). */
const char *sdcard_root(void);
