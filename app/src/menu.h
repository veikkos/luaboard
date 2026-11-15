/*
 * Home menu: the installed apps and Settings, driven by the touch buttons
 * (A up, C down, B open, hold B = back) and taps on the screen. Holding B
 * also leaves a running app. Owns the screen whenever no script does.
 */
#pragma once

#include <stdbool.h>

void menu_init(void);
/** Stop any script and show the app list. */
void menu_show(void);
/** A short message in the footer for a few seconds. */
void menu_toast(const char *msg);
/** Call from the main loop: buttons, redraws, leaving apps. */
void menu_tick(void);
/** The menu is installing apps from the marketplace (App updates, Get apps). */
bool menu_installing(void);
