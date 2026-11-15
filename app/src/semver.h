/*
 * App versions: semver, read leniently. "1.2.3", "1.2" and "1" (missing
 * parts are 0), an optional "-pre-release" and "+build" (ignored, so a dev
 * build "1.0.0+a1b2c3d-dirty" is 1.0.0). Pure logic, no allocation.
 */
#pragma once

#include <stdbool.h>

typedef struct {
    long major, minor, patch;
    char pre[24];                   /* "" for a release */
} semver_t;

/** Read a version. False for anything else ("", "v1.0.0", "1.2.3.4"). */
bool semver_parse(const char *s, semver_t *v);

/** <0, 0 or >0 as a is older than, the same as, or newer than b. A version
 *  that can't be read (or NULL) is older than every readable one. */
int semver_cmp(const char *a, const char *b);
