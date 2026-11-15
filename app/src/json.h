/* A small JSON reader for the flat objects the firmware reads itself:
 * app.json (appmeta.c) and the release manifest (update_manifest.c). No
 * allocation and no tree: the caller walks the top-level members and takes
 * the values it wants. Lua apps have their own (app/lua/json.lua).
 *
 * Text values come out as UTF-8 cut to the buffer at a whole character
 * (nothing after the first one that doesn't fit). Broken UTF-8 and half a
 * \u surrogate pair become ?, control characters (\n, \t, \u0001...) a
 * space: the values are shown on one screen line or wrapped. */
#pragma once

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    const char *p, *end;
    const char *err;        /* the first problem, NULL while there is none */
    int members;            /* of the top-level object, for json_member */
} json_t;

void json_init(json_t *j, const char *text, size_t len);

/** Set j->err (if it has none yet) and return false. */
bool json_fail(json_t *j, const char *why);

/** The next character after any white space, without taking it; 0 at the end. */
char json_peek(json_t *j);

/** The next member of the top-level object: its key into key[cap] ("" if
 *  it doesn't fit, so it matches nothing) and the ':' after it. Then read
 *  its value. False after the closing '}', or with j->err set if the text
 *  isn't a well-formed object. */
bool json_member(json_t *j, char *key, size_t cap);

/** A string value into out[cap] (out may be NULL); *cut (may be NULL) says
 *  whether it didn't fit whole. */
bool json_string(json_t *j, char *out, size_t cap, bool *cut);

/** Any value. A string's text, or a number, true, false or null as written,
 *  goes into out[cap] (out may be NULL); an array or object gives "". */
bool json_value(json_t *j, char *out, size_t cap);

/** A value that must be a whole number 0..999999999 (not a string). */
bool json_whole(json_t *j, long *out);

/** An object value: *inner reads its members with json_member, and j
 *  moves past it. False (j failed) if the value isn't an object. */
bool json_object(json_t *j, json_t *inner);

/** Walk an array value: call with *first true at the '['; each true return
 *  is positioned at the next item, which the caller reads before calling
 *  again. False after the ']' (or with j->err set). */
bool json_array_next(json_t *j, bool *first);
