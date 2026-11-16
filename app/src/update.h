/*
 * Firmware releases: the update check. With Wi-Fi joined and the setting
 * on, the release manifest (manifest-<board>.json of the latest release) is fetched a minute after
 * Wi-Fi first comes up, then every UPDATE_CHECK_EVERY_MS, in the background
 * (httpc, stepped from the main loop). A newer release for this board is
 * kept in RAM until the next check says otherwise.
 *
 * Events: "update available ver=<v>" when a newer release is first known,
 * "update manual format=<n>" when the release's manifest is in a later format,
 * "update check-failed <why>" when a check fails (it comes again in 10
 * minutes, then in an hour, then daily).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "update_manifest.h"

/** At boot, after net_init. */
void update_init(void);
/** In the main loop: start a check when one is due, move one along. */
void update_poll(void);
/** A newer release for this board, or NULL. */
const update_manifest_t *update_available(void);
/** Check now (Settings > Check now), whatever the schedule and the setting
 *  say, if Wi-Fi is up and none is under way. */
void update_check_now(void);
/** A check is under way. */
bool update_checking(void);
/** Why the last check failed, "" if it didn't. */
const char *update_error(void);
/** The newest release's manifest is in a later format than this firmware
 *  reads: there is a newer firmware, which it can't install by itself. */
bool update_manual(void);
/** Install the release update_available() gives: its image is downloaded
 *  (update_poll) straight into the other app slot, with the update screen
 *  up, then checked and booted (ota.c). False, and why in err, if it can't
 *  start; a download that fails later puts the menu back with why. */
bool update_install(char *err, size_t cap);
/** After settings_set_update_check: off drops a check in flight and the
 *  release known; on checks a minute later (with Wi-Fi up). */
void update_setting_changed(void);
