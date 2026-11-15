/*
 * Turning the device off. Asleep (sleep.c) everything keeps running; off,
 * the board is powered down and comes back with a boot: by the power key,
 * or by itself when an app asked to be woken (sys.off(seconds)).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "wake_time.h"

/** At boot, after clock_init(): works out why the device started and
 *  disarms the clock chip's wake-up. */
void power_init(void);
/** Why the device started: "power" (the power key, USB plugged in, power
 *  applied), "timer" (the wake-up time of sys.off) or "reset" (a restart,
 *  an update, a crash). */
const char *power_boot_reason(void);
/** Turn off once no Lua call is running (power_poll does it). seconds 0:
 *  until the power key; else the device is on again by then. by: "app" or
 *  "setting", for the event. Not WAKE_OK: nothing is asked for. */
wake_err_t power_off_request(uint32_t seconds, const char *by);
/** Main loop: acts on a request, and on the Turn off after setting: asleep
 *  that long on battery, the device turns itself off, unless an app keeps
 *  it on. */
void power_poll(void);
/** An app works in the dark (a countdown, a logger): the Turn off after
 *  setting doesn't turn the device off under it. Off when a script ends. */
void power_keep_on(bool on);
bool power_kept_on(void);
/** On USB power the board isn't powered down (it would drop the PC link
 *  and gain nothing): off is acted out. The app is stopped, the screen is
 *  off, and at the power key or the wake-up time the same script starts
 *  again in a fresh Lua state, as after a boot. True meanwhile: the main
 *  loop runs no script and no menu. */
bool power_acting_off(void);
/** The app that was running when the firmware turned the device off: the
 *  next boot starts it again in place of the autostart app, whichever way
 *  the device came on. Read once: the note is gone after this call. */
bool power_resume_app(char *app, size_t cap);
