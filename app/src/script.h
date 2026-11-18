/*
 * Lua script host.
 *
 * Lifecycle of a script:
 *   script_run("main.lua")  -> fresh VM, run top-level chunk, call setup()
 *   script_tick()           -> (from main loop) run due timers, call loop(dt)
 *   script_stop()           -> stop calling loop()/timers; VM kept for EVAL
 *
 * Safety: the VM has a hard memory cap (LUA_HEAP_LIMIT) and a watchdog that
 * aborts any single call into Lua running longer than LUA_WATCHDOG_MS
 * (LUA_WATCHDOG_HARD_MS counting slow C calls).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lua.h"

void script_init(void);
/** Start an app or script. name: "<app>" (its main.lua), "<app>/<file>.lua",
 *  or "<file>.lua" in the default app. On failure writes a message to err
 *  and shows the error screen. */
bool script_run(const char *name, char *err, size_t errlen);
/** Resolve a name as script_run() does into "<app>/<file>". */
bool script_resolve(const char *name, char *out, size_t cap);
/** The app "<file>.lua" refers to: the one last uploaded to or run. */
void script_set_default_app(const char *app);
/** App of the current (or last) script, "" if none. */
void script_app(char *out, size_t cap);
/** sys.exit() (exit true) and sys.off(): end the running Lua call at once
 *  (an app's pcall can't keep it going) and run nothing more of the app.
 *  sys.exit() then stops it; after sys.off() power_poll() does. Raises:
 *  call as `return script_leave(L, ...)`. */
int script_leave(lua_State *L, bool exit);
void script_stop(void);
bool script_running(void);
const char *script_current(void);  /* "<app>/<file>", "" if none */
/** Last script started (even if it failed or stopped), "" if none. */
const char *script_last(void);
void script_tick(void);
/** Milliseconds until the next tick is due: 0 if it is, UINT32_MAX if no
 *  script runs. */
uint32_t script_next_due_ms(void);
/** REPL: evaluate code in the current VM. Result or error text goes to out. */
bool script_eval(const char *code, char *out, size_t outlen);
void script_heap_stats(size_t *used, size_t *peak, size_t *limit);

/* For the API bindings. */
struct lua_State;
/** After a slow C call that started at hw_millis() == since (an image
 *  decode, a TLS step, sys.delay): don't count its time, up to max_ms,
 *  against the watchdog. No call can run past LUA_WATCHDOG_HARD_MS: raises
 *  the watchdog error if the call is out of time. */
void script_watchdog_credit(struct lua_State *L, uint32_t since, uint32_t max_ms);
void script_set_rate(int hz);
int  script_get_rate(void);
