/* The release manifest (manifest-<board>.json, written by .github/workflows/release.yml)
 * and the version comparison behind "an update is available". Pure logic:
 * no allocation, no networking. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define UPDATE_NOTES_MAX 128

typedef struct {
    int format;
    char version[32];
    char board[32];
    char url[256];
    size_t size;
    uint8_t sha256[32];
    int lua_api, lua_api_min;
    char notes[UPDATE_NOTES_MAX];   /* "" if none */
} update_manifest_t;

/** Parse the `len` bytes of manifest JSON. Returns false and writes a short
 *  reason to `err` (if `cap` > 0) when it is malformed, of a format this
 *  firmware doesn't know, or lacks or misstates a required field. For
 *  another format m->format tells which (the rest isn't read; `format` is a
 *  manifest's first field), else it is 1 or 0. */
bool update_manifest_parse(const char *json, size_t len, update_manifest_t *m, char *err, size_t cap);

/** True when `release` ("vX.Y.Z") is strictly newer than `running` (what
 *  `git describe` gave: "vX.Y.Z", "vX.Y.Z-N-gHASH[-dirty]", "vX.Y.Z-dirty",
 *  "vX.Y.Z-rc1", or a bare hash). A running version that is not a tag is
 *  never offered anything. */
bool update_is_newer(const char *release, const char *running);
