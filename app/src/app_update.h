/*
 * Apps from the marketplace (luaboard-marketplace): its index.json is
 * fetched on the update check's schedule (a minute after Wi-Fi first comes
 * up, then daily, with the "check for updates" setting on) and kept in
 * PSRAM. Installed apps whose name is in it and whose app.json version is
 * older than the newest version this firmware runs have an update. The
 * menu offers those, and every app of the index under "Get apps".
 *
 * Events: "app updates n=<n>" when the number of apps with an update
 * changes, "apps check-failed <why>" when a check fails.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "apps_index.h"

/** At boot, after vfs_init and net_init. */
void app_update_init(void);
/** In the main loop: start a check when one is due, move one along. */
void app_update_poll(void);
/** The "check for updates" setting changed. */
void app_update_setting_changed(void);
/** The user opened "Get apps": check now if no index is known yet or the
 *  last check is over an hour old (Wi-Fi permitting, whatever the setting). */
void app_update_check_now(void);
/** Check now, whatever the schedule, the setting and the last check
 *  (Settings > Check now), Wi-Fi permitting. */
void app_update_check_force(void);
/** A check is under way. */
bool app_update_checking(void);
/** There is a marketplace address (always on the device; --set apps_url in
 *  the simulator). */
bool app_update_configured(void);

/** The index from the last check that worked, or NULL. */
const apps_index_t *app_update_index(void);
/** Its text, which the entries' files_at point into. */
const char *app_update_text(size_t *len);
/** Why the last check failed (short, for the screen), "" if it didn't. */
const char *app_update_error(void);

/** How many installed apps have an update. */
int app_update_count(void);
/** The i-th of them (0..count-1), in the index's order. */
const apps_entry_t *app_update_at(int i);
/** The newer version of an installed app, or NULL. */
const apps_entry_t *app_update_for(const char *app);

/* Installing an app from the index (an update, or one not installed yet):
   its files are fetched one at a time (from app_update_poll), only those
   that differ from the installed ones, checked against their size and
   sha256, written aside and swapped in at once (vfs_stage_*). A failure
   leaves the installed version as it was. The app's data stays.
   Events: "app install <app> ok" or "app install <app> failed <why>". */

/** Start installing app. False (why in err) if one is under way, there's
 *  no index, the app isn't in it, or it is running. */
bool app_install_start(const char *app, char *err, size_t cap);
bool app_install_busy(void);
/** The app being (or last) installed. */
const char *app_install_app(void);
/** How far it is, 0..100, and the file it is on. */
int app_install_pct(void);
const char *app_install_file(void);
/** After an install: NULL if it worked, else a short reason (one screen line). */
const char *app_install_result(void);
