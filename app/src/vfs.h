/*
 * App storage. Files are addressed as "<app>/<file>" (one level).
 *
 *   flash: installed apps, a LittleFS partition mounted at APPS_MOUNT
 *          (/apps/<app>/<file>); read when needed, not kept in RAM.
 *   RAM:   uploads that aren't installed yet. A RAM file shadows the
 *          installed one with the same path; INSTALL moves it to flash.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef enum {
    VFS_INSTALLED,   /* in flash only */
    VFS_RAM,         /* uploaded, never installed: gone after a restart */
    VFS_MODIFIED,    /* installed, with newer uploads in RAM */
} vfs_app_state_t;

typedef struct {
    char name[32];
    int files;
    size_t bytes;
    vfs_app_state_t state;
} vfs_app_t;

/** Mount the apps partition (formatted the first time). */
void vfs_init(void);

bool vfs_valid_app(const char *app);
/** "app/file": a valid app name, one '/', a valid file name. */
bool vfs_valid_path(const char *path);
/** "app/file" -> app (all of path if it has no '/'). */
void vfs_app_of(const char *path, char *app, size_t cap);

/** True if another app has the same name in other letter case ("Spotify"
 *  vs "spotify"); its name goes to `other`. Such a name is refused: on a
 *  file system that ignores case (the simulator on macOS) the two would share
 *  one folder, while secrets go by the exact name. */
bool vfs_app_case_clash(const char *app, char *other, size_t cap);

/* App data (the Lua `store` module): one file per app, "<mount>/.data/<app>",
 * kept apart from the app's files so uploads and installs never touch it. */
/** Size of the app's data file, or -1 if it has none. */
long vfs_data_size(const char *app);
/** Read the app's data file into buf (cap bytes); its length, or -1. */
long vfs_data_read(const char *app, uint8_t *buf, size_t cap);
/** Replace the app's data file (atomically); len 0 deletes it. */
esp_err_t vfs_data_save(const char *app, const uint8_t *data, size_t len);
/** Delete the app's data (uninstall, Clear data). */
void vfs_data_clear(const char *app);
/** Saves come at most once a second; a later one waits in RAM. Write it now
 *  (a script ended), or once its time has come (from the main loop). */
void vfs_data_flush(void);
void vfs_data_poll(void);

/** Store a file in RAM (an upload). Takes a copy. Refuses a name that
 *  clashes with another app's in letter case. */
esp_err_t vfs_write(const char *path, const uint8_t *data, size_t len);
/** File contents (RAM copy first, then flash) in a new malloc'd buffer the
 *  caller frees, or NULL if there's no such file. */
uint8_t *vfs_load(const char *path, size_t *len);
bool vfs_exists(const char *path);
/** Delete from RAM and flash. */
esp_err_t vfs_delete(const char *path);

typedef void (*vfs_file_cb)(const char *path, size_t size, uint32_t crc, void *ctx);
/** Every file as an app would see it (RAM shadowing flash), sorted. Returns the count. */
int vfs_list(vfs_file_cb cb, void *ctx);
/** Apps, sorted by name. Returns the count (at most max). */
int vfs_apps(vfs_app_t *out, int max);
/** How many apps vfs_apps() lists, without looking at their files. */
int vfs_app_count(void);
bool vfs_app_exists(const char *app);
/** One app's state as vfs_apps() gives it; false if there's no such app. */
bool vfs_app_state(const char *app, vfs_app_state_t *state);

/** Write an app's RAM files to flash and drop the RAM copies. */
esp_err_t vfs_install(const char *app);
/** Install every app that has RAM files. *count = apps installed. */
esp_err_t vfs_install_all(int *count);
/** Delete an app from RAM and flash. */
esp_err_t vfs_uninstall(const char *app);
/** Delete everything, RAM and flash. */
esp_err_t vfs_format(void);

/* A whole new version of an app, written aside and swapped in at once (the
   marketplace's installs and updates). A power cut leaves the old version or
   the new one: vfs_init finishes or undoes the swap. The app's data (store,
   secrets) isn't touched. */

/** Start an empty stage for app (dropping an earlier one). */
esp_err_t vfs_stage_begin(const char *app);
/** Write one file of the new version. ESP_ERR_NO_MEM: no room. */
esp_err_t vfs_stage_write(const char *app, const char *name, const void *data, size_t len);
/** Take the installed file name over into the stage as it is. */
esp_err_t vfs_stage_keep(const char *app, const char *name);
/** Swap the stage in: it becomes the app, the old version goes, and with it
 *  the app's uploads waiting in RAM. */
esp_err_t vfs_stage_commit(const char *app);
/** Drop the stage; the installed version stays. */
void vfs_stage_abort(const char *app);

void vfs_usage(size_t *ram_used, size_t *flash_used, size_t *flash_total);
/** Changes whenever the set of apps or their state changes (for the menu). */
uint32_t vfs_generation(void);
