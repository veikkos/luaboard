/*
 * Firmware-owned screens: boot splash and the script error screen.
 * Scripts own the display while they run; these appear when no script does.
 */
#pragma once

#include <stdbool.h>

void screen_splash(const char *status);
/** The firmware update screen: a title, a status line and a progress bar
 *  (pct 0..100). */
void screen_update(const char *title, const char *line, int pct);
/** The same with another line at the bottom than the firmware update's. */
void screen_progress(const char *title, const char *line, int pct, const char *note);
/** At boot after a reset that wasn't asked for: the firmware crashed
 *  (`reason` as hw_reset_reason gives it), or with `power` the power dipped.
 *  `cause` and `where` (the innermost frame) from the crash record, or NULL. */
void screen_restarted(const char *reason, bool power, const char *cause, const char *where);
/** The Restarted screen's bottom line: going on in `secs` seconds, or a tap. */
void screen_restarted_countdown(int secs);
/** Show a script failure: file/line, message, how to recover. */
void screen_error(const char *script, const char *message);
/** True while the error screen is up (cleared when a script starts drawing). */
bool screen_error_active(void);
void screen_error_clear(void);
/** Neutral notice about a script (e.g. it drew nothing). The script's first
 *  drawing call clears it. */
void screen_notice(const char *title, const char *message);
bool screen_notice_active(void);
void screen_notice_clear(void);
