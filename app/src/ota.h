/*
 * Firmware updates (OTA): a new app image is pushed over the PC link (USB or
 * Wi-Fi, the OTA command) into the app slot that isn't running, checked, and
 * booted. The old firmware stays in the other slot until the next update.
 *
 * The new firmware is watched for boot loops: a crash reset early in its
 * boot (before ota_boot) makes the bootloader go back to the old one; after
 * that, OTA_ROLLBACK_BOOTS boots in a row that end in a firmware crash
 * (panic, watchdog) make it go back itself. It's proven, and no longer
 * watched, once it has run OTA_STABLE_MS; a reset that isn't a crash starts
 * the count over.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** The running firmware's version: the build's `git describe`. */
const char *fw_version(void);

/** Early at boot, once NVS is up: confirm this firmware to the bootloader,
 *  count a crash boot of a new update and go back to the old firmware after
 *  too many. */
void ota_boot(void);
/** True on the first boot of a new update (to say so on the screen). */
bool ota_just_updated(void);
/** In the main loop: reboot after an update, note when one is proven. */
void ota_poll(void);

/** Start an update of `size` bytes with SHA-256 `sha`: stops the app and shows
 *  the update screen. False (and why in err) if it can't. */
bool ota_begin(size_t size, const uint8_t sha[32], char *err, size_t cap);
/** The next bytes of the image: takes up to the bytes still expected and
 *  returns how many it took. A write error is remembered for ota_finish. */
size_t ota_write(const void *data, size_t len);
/** The bytes of the image in so far, of `*size`. All in: ready for ota_finish. */
size_t ota_received(size_t *size);
/** All bytes are in: check them and boot the new image next. On success
 *  `msg` is its version and the device reboots shortly (ota_poll); else
 *  `msg` says why and the device goes back to its menu. */
bool ota_finish(char *msg, size_t cap);
/** Give up on an update in progress (the upload stopped or its link went away). */
void ota_abort(const char *why);
/** True while an update owns the device: being written, or about to reboot. */
bool ota_active(void);
/** The clients were sent away for the reboot, which comes in a moment: a
 *  new one now would find the old firmware (and take it for a rollback). */
bool ota_restarting(void);

/** The app slot running ("ota_0", "ota_1") and its state: "ok", or
 *  "rolledback" (an update kept crashing, this is the old firmware again). */
const char *ota_slot(void);
const char *ota_state(void);
