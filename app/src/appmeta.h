/*
 * App metadata: an optional app.json in the app's folder.
 *
 *   { "title": "Spotify", "description": "Now playing, with cover art",
 *     "version": "1.2", "author": "Veikko", "lua_api": 1 }
 *
 * Only top-level string (or number) values are read; anything else is
 * skipped. "lua_api" is the Lua API version the app was written for
 * (app_config.h), a whole number. Text is UTF-8; text longer than its
 * field (APP_*_MAX) is cut at a whole character and ends in "…". A missing
 * or broken file just means no metadata.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "app_config.h"

#define APP_META_FILE "app.json"
#define APP_META_MAX  4096          /* bigger files are ignored */

typedef struct {
    char title[APP_TITLE_MAX + 1];
    char description[APP_DESC_MAX + 1];
    char version[32];               /* room for a dev build's "1.0.0+a1b2c3d-dirty" */
    char author[APP_AUTHOR_MAX + 1];
    int lua_api;                    /* 0 = not given (means 1), -1 = not a whole number >= 1 */
} app_meta_t;

/** Read <app>/app.json. Returns false (and an empty *m) if there's none or
 *  it isn't a JSON object. */
bool app_meta_load(const char *app, app_meta_t *m);

/** Parse metadata from JSON text (for app_meta_load and tests). */
bool app_meta_parse(const char *json, size_t len, app_meta_t *m);

/** Whether this firmware runs an app with this metadata: NULL if it does,
 *  else why not (in buf), such as "needs newer firmware". */
const char *app_meta_api_problem(const app_meta_t *m, char *buf, size_t cap);

/** Whether firmware running Lua API versions api_min..api runs an app with
 *  this metadata (no lua_api means 1; a broken one runs nowhere). */
bool app_meta_runs_on(const app_meta_t *m, int api_min, int api);
