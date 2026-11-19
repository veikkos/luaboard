/*
 * The marketplace's index.json (written by luaboard-marketplace's
 * tools/build_index.py): every app and the versions a board can install.
 *
 *   {"format": 1, "apps": {"<name>": {"title", "description", "author",
 *     "license", "needs_setup", "versions": [{"version", "lua_api", "commit",
 *     "files": [{"path", "size", "sha256"}], "other_files": [...]}]}}}
 *
 * Parsing keeps, for each app, the newest version this firmware runs (its
 * lua_api in api_min..api); apps with none are left out. A version's files
 * are read again from the text when an install needs them (files_at), so
 * the caller keeps the text. Pure logic, no allocation.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app_config.h"

#define APPS_INDEX_FORMAT    1      /* the format this firmware reads; a later one: newer_format */
/* The marketplace's checks keep its apps and index within these limits,
   APPS_INDEX_MAX and app.json's text sizes (its tools/board.json, held to
   them by fw-unit --marketplace): what is past one is left out here
   without a word. */
#define APPS_INDEX_MAX_APPS  128
#define APPS_INDEX_MAX_FILES 32     /* board files of one app version */
#define APPS_NAME_MAX        24     /* = VFS_APP_NAME_MAX (checked in app_update.c) */
#define APPS_PATH_MAX        32     /* = VFS_FILE_NAME_MAX */
#define APPS_FILE_MAX        (256 * 1024)   /* = VFS_FILE_MAX: bigger files can't be loaded */

typedef struct {
    char path[APPS_PATH_MAX];
    uint32_t size;
    uint8_t sha256[32];
} apps_file_t;

typedef struct {
    char name[APPS_NAME_MAX];
    char title[APP_TITLE_MAX + 1], description[APP_DESC_MAX + 1];   /* cut ones end in "…" */
    char author[APP_AUTHOR_MAX + 1], license[APP_LICENSE_MAX + 1];
    char version[24];
    int lua_api;
    char commit[41];
    bool needs_setup;               /* has keys to set before it works (a login, an API key) */
    uint32_t files_at;              /* where its "files" array starts in the text */
} apps_entry_t;

typedef struct {
    int n;
    bool newer_format;              /* written for a later firmware: nothing to offer */
    apps_entry_t app[APPS_INDEX_MAX_APPS];
} apps_index_t;

/** Read the index text. False (with why in err) if it is broken. */
bool apps_index_parse(const char *text, size_t len, int api_min, int api, apps_index_t *out, char *err, size_t cap);

/** The files of e's version: how many went into out[cap] (out may be NULL),
 *  -1 if the list is broken, longer than cap or has a file over
 *  APPS_FILE_MAX. */
int apps_index_files(const char *text, size_t len, const apps_entry_t *e, apps_file_t *out, int cap);

/** The app called name, or NULL. */
const apps_entry_t *apps_index_find(const apps_index_t *ix, const char *name);
