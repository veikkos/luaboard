/*
 * Device settings kept in NVS: display brightness, screen off, speaker
 * volume, the autostart app, the time zone and the update check.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

void settings_init(void);

/* One value of the firmware's own in NVS, for the modules that keep a
 * little there (the boot counter, the update's state, the pairing key...). */
/** Read it into out (at most cap bytes); its length, 0 if there is none or
 *  it is longer than cap. */
size_t settings_load(const char *key, void *out, size_t cap);
/** Store len bytes, or with len 0 erase it (not being there is fine). */
esp_err_t settings_store(const char *key, const void *data, size_t len);
/** settings_load, then erase it: a value taken once. */
size_t settings_take(const char *key, void *out, size_t cap);

/** Brightness in % (BRIGHTNESS_MIN..100): the menu's, and every app's at start. */
int  settings_brightness(void);
esp_err_t settings_set_brightness(int percent);

/** Minutes without a touch before the screen goes off; 0 = never. */
int  settings_screen_off(void);
esp_err_t settings_set_screen_off(int minutes);

/** Minutes asleep (the screen off) before the device turns itself off, on
 *  battery; 0 = never. */
int  settings_turn_off(void);
esp_err_t settings_set_turn_off(int minutes);

/** Speaker volume in %, 0 = off: every sound's loudness is a share of it. */
int  settings_volume(void);
esp_err_t settings_set_volume(int percent);

/** App started at boot instead of the menu; "" = none. */
const char *settings_autostart(void);
esp_err_t settings_set_autostart(const char *app);

/** Time zone as a POSIX TZ string (TZ_DEFAULT unless set). Use clock_set_tz()
 *  to change it: that checks the string and applies it. */
const char *settings_timezone(void);
esp_err_t settings_set_timezone(const char *tz);

/** Look for new firmware releases (update.c); on unless turned off. Call
 *  update_setting_changed() after changing it. */
bool settings_update_check(void);
esp_err_t settings_set_update_check(bool on);
