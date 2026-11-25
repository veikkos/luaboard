/*
 * Sleep: the screen goes off after the Sleep after setting's minutes without
 * a touch, or at once (a long press of the power key, Settings > Sleep now,
 * an app's disp.sleep). Whatever ran keeps running. A touch or the power
 * key wakes it, and does nothing else: the touch that woke it isn't a tap.
 * On battery the chip may go into light sleep meanwhile.
 *
 * buttons.c drives it, as it reads the touch controller and the power key.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/** The screen off now. */
void sleep_now(void);
/** True while the screen is off. */
bool sleep_active(void);
/** A touch or key at `now`: restarts the idle time. True if it woke the
 *  screen (and is to be ignored). */
bool sleep_activity(uint32_t now);
/** Keep the screen on whatever the Screen off setting says (an app asks
 *  for it; Sleep now and the power key still turn it off). */
void sleep_keep_awake(bool on);
bool sleep_kept_awake(void);
/** Turn the screen off if it has been idle for the setting's time. */
void sleep_tick(uint32_t now);
/** hw_millis() of the last touch or key (now, while an app keeps the screen
 *  awake): what the Sleep after and Turn off after settings count from. */
uint32_t sleep_idle_since(void);
/** Asleep with nothing in the way of the chip's light sleep: on battery,
 *  no sound playing, no update running, Bluetooth idle. The main loop
 *  slows down while it holds (main.c). */
bool sleep_light_allowed(void);
