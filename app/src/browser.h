/*
 * Browser-assisted login (OAuth and similar): a script asks a connected tool
 * (studio, CLI) to open a URL in the computer's browser and to catch the
 * redirect to a loopback address. The tool answers with the redirect's query
 * string. One request at a time; it stays pending (and is re-announced to
 * tools that connect later) until answered, cancelled or the script ends.
 *
 *   EVT browser <id> <app> <redirect> <url> <title>   a new request (<app>: the
 *                                                     app asking, set by the firmware)
 *   EVT browser-done <id> ok|cancelled            it's over (close any prompt)
 */
#pragma once

#include <stdbool.h>

typedef enum { BROWSER_PENDING, BROWSER_DONE, BROWSER_CANCELLED, BROWSER_UNKNOWN } browser_state_t;

/** Start a request (replaces a pending one). Returns its id, or 0 + err. */
int browser_start(const char *app, const char *url, const char *redirect, const char *title, const char **err);
/** State of request `id`; for DONE, *result is the query string. */
browser_state_t browser_poll(int id, const char **result);
void browser_cancel(int id);
/** A tool's answer. False if `id` isn't the pending request. */
bool browser_answer(int id, const char *query);
/** The pending request as "<id> <app> <redirect> <url> <title>", or false. */
bool browser_pending(char *buf, int len);
/** The script ended: cancel whatever is pending and forget any answer. */
void browser_reset(void);
