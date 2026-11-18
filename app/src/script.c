#include "script.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_config.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "gfx.h"
#include "hw.h"
#include "lauxlib.h"
#include "lua.h"
#include "builtin_lua.h"
#include "lua_api.h"
#include "appmeta.h"
#include "lua_sound.h"
#include "sound.h"
#include "lua_net.h"
#include "lua_web.h"
#include "lua_store.h"
#include "lua_i2c.h"
#include "lua_sd.h"
#include "lua_sensors.h"
#include "lua_uart.h"
#include "lua_ble.h"
#include "ble.h"
#include "browser.h"
#include "buttons.h"
#include "lualib.h"
#include "protocol.h"
#include "screens.h"
#include "vfs.h"

static const char *TAG = "script";

static lua_State *L;
static bool s_running;
static char s_current[VFS_MAX_NAME];
static int s_tick_ref = LUA_NOREF;
static uint32_t s_deadline;      /* watchdog deadline (ms) for the current call */
static uint32_t s_hard_deadline; /* ... which credits can't push past */
static uint32_t s_period_ms = 1000 / LUA_DEFAULT_RATE_HZ;
static uint32_t s_next_due, s_last_tick;
static size_t s_heap_used, s_heap_peak;
static uint32_t s_started;       /* when the current script started (ms) */
static bool s_noticed;           /* "nothing drawn" check done for this run */
static bool s_returned_table;    /* the main chunk returned a table (a module?) */
static bool s_exit_requested;    /* sys.exit() was called */
static bool s_halted;            /* sys.exit()/sys.off(): nothing more of the app runs */
static bool s_leaving;           /* ... and the Lua call is being unwound */
static bool s_call_left;         /* the last pcall_wd ended by sys.exit()/sys.off() */
static char s_default_app[VFS_APP_NAME_MAX]; /* "RUN file.lua" means this app's file */

#define NOTHING_DRAWN_MS 1500    /* running this long without drawing: say so */

/* ---------------------------------------------------------------- memory */

static void *l_alloc(void *ud, void *ptr, size_t osize, size_t nsize)
{
    (void)ud;
    size_t old = ptr ? osize : 0;
    if (nsize == 0) {
        heap_caps_free(ptr);
        s_heap_used -= old;
        return NULL;
    }
    if (s_heap_used - old + nsize > LUA_HEAP_LIMIT) return NULL; /* -> "not enough memory" */
    /* PSRAM: plain malloc keeps blocks under SPIRAM_MALLOC_ALWAYSINTERNAL
       (16 KB) in internal RAM, which Wi-Fi, lwIP and Bluetooth need. */
    void *np = heap_caps_realloc(ptr, nsize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!np) return NULL;
    s_heap_used = s_heap_used - old + nsize;
    if (s_heap_used > s_heap_peak) s_heap_peak = s_heap_used;
    return np;
}

void script_heap_stats(size_t *used, size_t *peak, size_t *limit)
{
    if (used) *used = s_heap_used;
    if (peak) *peak = s_heap_peak;
    if (limit) *limit = LUA_HEAP_LIMIT;
}

/* -------------------------------------------------------------- watchdog */

#define WD_HOOK_COUNT 1000
static void wd_hook(lua_State *Ls, lua_Debug *ar);
#define WD_MSG "watchdog: call exceeded %d ms (use loop()/timers instead of busy loops)"
#define LEAVE_MSG "(leaving the app)"

/* Set once the current call is over time, until pcall_wd returns. The error
   is an ordinary Lua error, so pcall/xpcall in the app can catch it; from
   then on every thread that runs gets the hook on every instruction and
   raises again at once, so no retry loop can keep the call going. */
static bool s_tripped;

static void wd_trip(lua_State *Ls)
{
    s_tripped = true;
    lua_sethook(Ls, wd_hook, LUA_MASKCOUNT, 1);
    luaL_error(Ls, WD_MSG, LUA_WATCHDOG_MS);
}

static void wd_check(lua_State *Ls)
{
    if (s_tripped || (int32_t)(hw_millis() - s_deadline) > 0) wd_trip(Ls);
}

/* Every 1000 instructions: the watchdog, and the touch (read at its own
   rate, so a long-running call doesn't make touches late or merge them). */
static void wd_hook(lua_State *Ls, lua_Debug *ar)
{
    (void)ar;
    if (s_tripped) wd_trip(Ls);
    if (s_leaving) {                             /* as a tripped watchdog: again and again */
        lua_sethook(Ls, wd_hook, LUA_MASKCOUNT, 1);
        lua_pushliteral(Ls, LEAVE_MSG);
        lua_error(Ls);
    }
    /* A coroutine left on every-instruction from an earlier call. */
    if (lua_gethookcount(Ls) != WD_HOOK_COUNT) lua_sethook(Ls, wd_hook, LUA_MASKCOUNT, WD_HOOK_COUNT);
    buttons_sample();
    wd_check(Ls);
}

/* xpcall, with the app's message handler skipped once the watchdog has
   tripped. Lua runs a message handler with hooks off when the error comes
   from a hook, so a handler that loops would never be stopped. */
static int wd_msgh(lua_State *Ls)
{
    if (s_tripped || s_leaving) return 1;        /* the error as it is */
    lua_pushvalue(Ls, lua_upvalueindex(1));
    lua_insert(Ls, 1);
    lua_call(Ls, lua_gettop(Ls) - 1, 1);
    return 1;
}

static int xpcall_done(lua_State *Ls, int status, lua_KContext extra)
{
    if (status != LUA_OK && status != LUA_YIELD) {
        lua_pushboolean(Ls, 0);
        lua_pushvalue(Ls, -2);
        return 2;
    }
    return lua_gettop(Ls) - (int)extra;
}

static int l_xpcall(lua_State *Ls)
{
    int n = lua_gettop(Ls);
    luaL_checktype(Ls, 2, LUA_TFUNCTION);
    lua_pushvalue(Ls, 2);
    lua_pushcclosure(Ls, wd_msgh, 1);
    lua_replace(Ls, 2);
    lua_pushboolean(Ls, 1);                      /* as luaB_xpcall */
    lua_pushvalue(Ls, 1);
    lua_rotate(Ls, 3, 2);
    int status = lua_pcallk(Ls, n - 2, LUA_MULTRET, 2, 2, xpcall_done);
    return xpcall_done(Ls, status, 2);
}

static void wd_arm(void)
{
    s_tripped = s_leaving = false;
    uint32_t now = hw_millis();
    s_deadline = now + LUA_WATCHDOG_MS;
    s_hard_deadline = now + LUA_WATCHDOG_HARD_MS;
}

/* Credit only the time the C call really took, capped per call, and never
   past the hard deadline: a loop of cheap calls must still trip the watchdog.
   Checked right here too: the count hook fires only every 1000 instructions,
   which a loop around a slow call takes ages to get through. */
void script_watchdog_credit(lua_State *Ls, uint32_t since, uint32_t max_ms)
{
    uint32_t took = hw_millis() - since;
    if (took > max_ms) took = max_ms;
    s_deadline += took;
    if ((int32_t)(s_deadline - s_hard_deadline) > 0) s_deadline = s_hard_deadline;
    wd_check(Ls);
}

void script_set_rate(int hz)
{
    if (hz < 1) hz = 1;
    if (hz > LUA_MAX_RATE_HZ) hz = LUA_MAX_RATE_HZ;
    s_period_ms = 1000u / (uint32_t)hz;
}

int script_get_rate(void) { return (int)(1000u / s_period_ms); }

/* Protected call with watchdog and traceback-free error message. */
static bool pcall_wd(int nargs, int nresults, char *err, size_t errlen)
{
    wd_arm();
    int rc = lua_pcall(L, nargs, nresults, 0);
    bool tripped = s_tripped, left = s_leaving;
    if (tripped || left) {
        s_tripped = s_leaving = false;
        lua_sethook(L, wd_hook, LUA_MASKCOUNT, WD_HOOK_COUNT);
    }
    s_call_left = rc != LUA_OK && left && !tripped;
    if (rc == LUA_OK) return true;
    if (s_call_left) {                           /* sys.exit()/sys.off(): not an error */
        lua_pop(L, 1);
        if (err && errlen) err[0] = 0;
        return false;
    }
    const char *msg = lua_tostring(L, -1);
    /* A tripped call ends with the watchdog's error, whatever an app error
       handler made of it along the way. */
    if (tripped && err) snprintf(err, errlen, WD_MSG, LUA_WATCHDOG_MS);
    else if (err) snprintf(err, errlen, "%s", msg ? msg : "(error object is not a string)");
    lua_pop(L, 1);
    return false;
}

/* ------------------------------------------------------- print & require */

/* All arguments as one tab-separated string.: print() and the result of EVAL. */
static int join_results(lua_State *Ls)
{
    int n = lua_gettop(Ls);
    luaL_Buffer b;
    luaL_buffinit(Ls, &b);
    for (int i = 1; i <= n; i++) {
        if (i > 1) luaL_addchar(&b, '\t');
        luaL_tolstring(Ls, i, NULL);
        luaL_addvalue(&b);
    }
    luaL_pushresult(&b);
    return 1;
}

static int l_print(lua_State *Ls)
{
    join_results(Ls);
    proto_log(lua_tostring(Ls, -1));
    return 0;
}

/* require(name): loads "<name>.lua" (or name as-is) from the VFS, once. */
static int l_require(lua_State *Ls)
{
    const char *name = luaL_checkstring(Ls, 1);
    lua_getfield(Ls, LUA_REGISTRYINDEX, "script.loaded");
    lua_getfield(Ls, -1, name);
    if (lua_islightuserdata(Ls, -1))      /* still loading: a require loop */
        return luaL_error(Ls, "require loop: '%s' is still loading (it requires itself, directly or "
                              "through another module; an uploaded %s.lua also replaces a built-in one)", name, name);
    if (!lua_isnil(Ls, -1)) return 1;
    lua_pop(Ls, 1);
    lua_pushlightuserdata(Ls, (void *)l_require);   /* mark as loading */
    lua_setfield(Ls, -2, name);

    char fname[VFS_MAX_NAME + 8];
    size_t nl = strlen(name);
    if (nl > 4 && !strcmp(name + nl - 4, ".lua")) snprintf(fname, sizeof fname, "%s", name);
    else snprintf(fname, sizeof fname, "%s.lua", name);

    /* A file of the running app wins; otherwise a library built into the firmware. */
    char app[VFS_APP_NAME_MAX], path[VFS_APP_NAME_MAX + VFS_MAX_NAME + 8];
    script_app(app, sizeof app);
    snprintf(path, sizeof path, "%s/%s", app, fname);
    size_t len;
    uint8_t *own = vfs_load(path, &len);
    const char *src = own ? (const char *)own : builtin_lua(fname, &len);
    if (!src) {
        lua_pushnil(Ls); lua_setfield(Ls, -2, name);
        return luaL_error(Ls, "module '%s' not found (no file '%s' in app '%s')", name, fname, app);
    }

    /* "=http.lua" for a built-in library, "@http.lua" for the app's own
       file: both read "http.lua:12:" in messages, and sys._error tells
       them apart. */
    char chunk[VFS_MAX_NAME + 9];
    snprintf(chunk, sizeof chunk, "%c%s", own ? '@' : '=', fname);
    int rc = luaL_loadbufferx(Ls, src, len, chunk, "t");
    free(own);
    if (rc != LUA_OK) {
        lua_pushnil(Ls); lua_setfield(Ls, -3, name);     /* not loading any more */
        return lua_error(Ls);
    }
    lua_pushstring(Ls, name);
    if (lua_pcall(Ls, 1, 1, 0) != LUA_OK) {             /* a module that fails can be required again */
        lua_pushnil(Ls); lua_setfield(Ls, -3, name);
        return lua_error(Ls);
    }
    if (lua_isnil(Ls, -1)) { lua_pop(Ls, 1); lua_pushboolean(Ls, 1); }
    lua_pushvalue(Ls, -1);
    lua_setfield(Ls, -3, name); /* loaded[name] = result */
    return 1;
}

/* load(chunk, [name], [mode], [env]) with the mode forced to text. Lua
   bytecode is not verified when loaded: a crafted binary chunk can read and
   write memory outside the VM, so scripts may only load source code. */
static int l_load_text(lua_State *Ls)
{
    if (lua_gettop(Ls) < 3) lua_settop(Ls, 3);
    if (lua_gettop(Ls) > 4) lua_settop(Ls, 4);   /* an env given (even nil) stays */
    lua_pushliteral(Ls, "t");
    lua_replace(Ls, 3);
    int n = lua_gettop(Ls);
    lua_pushvalue(Ls, lua_upvalueindex(1));       /* the original load */
    lua_insert(Ls, 1);
    lua_call(Ls, n, LUA_MULTRET);
    return lua_gettop(Ls);
}

/* ------------------------------------------------------------- VM set-up */

static void open_lib(const char *name, lua_CFunction fn)
{
    luaL_requiref(L, name, fn, 1);
    lua_pop(L, 1);
}

/* The firmware's Lua modules: each puts its functions in a new VM, and
   gives back what a script took through it (pins, buses, sockets, Bluetooth
   connections, sound, a pending browser login, unsaved data) when the
   script ends. Given back in the reverse order: the pins (lua_api) last,
   after the buses and devices that use them. A new module goes here, with
   an e2e check that its state is back after a script (docs/LUA_API.md,
   "What carries over between scripts"). */
static const struct {
    void (*add)(lua_State *L);
    void (*release)(void);
} s_modules[] = {
    { lua_api_register, lua_api_release_hw },
    { lua_net_register, lua_net_close_all },
    { lua_web_register, browser_reset },
    { lua_store_register, vfs_data_flush },
    { lua_i2c_register, lua_i2c_release },
    { lua_uart_register, lua_uart_release },
    { lua_sensors_register, lua_sensors_release },
    { lua_sd_register, lua_sd_release },
    { lua_ble_register, ble_reset },
    { lua_sound_register, sound_stop },
};
#define N_MODULES (sizeof s_modules / sizeof *s_modules)

/* Everything a script can leave behind outside the Lua VM. Runs when a
   script starts, is stopped, or fails. (The VM itself is replaced on the
   next run; after STOP it is kept so EVAL can still inspect it.) */
static void release_script_resources(void)
{
    for (size_t i = N_MODULES; i-- > 0;) s_modules[i].release();
    buttons_reset();             /* the screen changes hands: a touch in progress isn't for the next one */
}

/* Close the VM. Its __gc finalizers run here, still as the app that owned it
   (s_current): callers change s_current only after this. */
static void vm_close(void)
{
    if (L) lua_close(L);
    L = NULL;
    release_script_resources();
}

static bool vm_create(char *err, size_t errlen)
{
    vm_close();
    s_tick_ref = LUA_NOREF;
    s_heap_peak = s_heap_used;
    L = lua_newstate(l_alloc, NULL);
    if (!L) { snprintf(err, errlen, "out of memory creating VM"); return false; }
    lua_sethook(L, wd_hook, LUA_MASKCOUNT, WD_HOOK_COUNT);

    /* Safe subset of the standard library: no io/os/debug/package. */
    open_lib(LUA_GNAME, luaopen_base);
    open_lib(LUA_TABLIBNAME, luaopen_table);
    open_lib(LUA_STRLIBNAME, luaopen_string);
    open_lib(LUA_MATHLIBNAME, luaopen_math);
    open_lib(LUA_UTF8LIBNAME, luaopen_utf8);
    open_lib(LUA_COLIBNAME, luaopen_coroutine);
    lua_pushnil(L); lua_setglobal(L, "dofile");   /* would touch the host FS */
    lua_pushcfunction(L, l_xpcall); lua_setglobal(L, "xpcall");   /* see wd_msgh */
    lua_pushnil(L); lua_setglobal(L, "loadfile");
    lua_getglobal(L, "load");                     /* source only, never bytecode */
    lua_pushcclosure(L, l_load_text, 1);
    lua_setglobal(L, "load");
    lua_getglobal(L, LUA_STRLIBNAME);             /* bytecode is useless without it */
    lua_pushnil(L); lua_setfield(L, -2, "dump");
    lua_pop(L, 1);

    for (size_t i = 0; i < N_MODULES; i++) s_modules[i].add(L);

    lua_newtable(L);
    lua_setfield(L, LUA_REGISTRYINDEX, "script.loaded");
    lua_register(L, "print", l_print);
    lua_register(L, "require", l_require);

    size_t plen;
    const char *prelude = builtin_lua("prelude", &plen);
    if (luaL_loadbuffer(L, prelude, plen, "=prelude") != LUA_OK ||
        !pcall_wd(0, 1, err, errlen)) {
        if (lua_gettop(L) > 0 && lua_isstring(L, -1)) snprintf(err, errlen, "%s", lua_tostring(L, -1));
        return false;
    }
    s_tick_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    script_set_rate(LUA_DEFAULT_RATE_HZ);
    return true;
}

void script_init(void)
{
    char err[128];
    if (!vm_create(err, sizeof err)) ESP_LOGE(TAG, "VM init failed: %s", err);
    s_running = false;
    s_current[0] = 0;
}

void script_set_default_app(const char *app)
{
    if (vfs_valid_app(app)) snprintf(s_default_app, sizeof s_default_app, "%s", app);
}

bool script_resolve(const char *name, char *out, size_t cap)
{
    size_t n = strlen(name);
    if (strchr(name, '/')) snprintf(out, cap, "%s", name);                   /* app/file.lua */
    else if (n > 4 && !strcmp(name + n - 4, ".lua")) {                       /* file.lua */
        if (!s_default_app[0]) return false;
        snprintf(out, cap, "%s/%s", s_default_app, name);
    } else snprintf(out, cap, "%s/main.lua", name);                          /* app */
    return vfs_valid_path(out);
}

void script_app(char *out, size_t cap)
{
    const char *slash = strchr(s_current, '/');
    snprintf(out, cap, "%.*s", slash ? (int)(slash - s_current) : 0, s_current);
}

/* setup(), if the script defines one. Runs under lua_pcall. */
static int call_setup(lua_State *Ls)
{
    lua_getglobal(Ls, "setup");
    if (lua_isfunction(Ls, -1)) lua_call(Ls, 0, 0);
    return 0;
}

static bool run_inner(const char *name, char *err, size_t errlen)
{
    /* The old app goes first, even if the new one then fails to start:
       otherwise it would keep running after its sockets, pins and links
       were let go. */
    s_running = false;
    vm_close();                  /* the old app's finalizers must not see the new app */
    s_exit_requested = s_halted = false;
    s_current[0] = 0;

    char path[VFS_MAX_NAME];
    if (!script_resolve(name, path, sizeof path)) {
        snprintf(err, errlen, "which app? '%s' (use <app>, <app>/<file>.lua, or upload to an app first)", name);
        return false;
    }
    snprintf(s_current, sizeof s_current, "%s", path);   /* its error screen can retry it */
    char app[VFS_APP_NAME_MAX];
    script_app(app, sizeof app);
    size_t len;
    uint8_t *src = vfs_load(path, &len);
    if (!src) {
        snprintf(err, errlen, vfs_app_exists(name) ? "app '%s' has no main.lua" : "no such app or file: %s", name);
        return false;
    }
    app_meta_t meta;             /* an app written for another Lua API doesn't start */
    app_meta_load(app, &meta);
    if (app_meta_api_problem(&meta, err, errlen)) { free(src); return false; }

    script_set_default_app(app);
    if (!vm_create(err, errlen)) { free(src); return false; }
    gfx_fill(0);                 /* start from a black screen, not the old script's picture */

    /* Chunk name: the file name only, so messages read "main.lua:11: ...",
       which the studio maps to that file. */
    char chunk[VFS_MAX_NAME + 1];
    snprintf(chunk, sizeof chunk, "@%s", strchr(path, '/') + 1);
    int rc = luaL_loadbufferx(L, (const char *)src, len, chunk, "t");
    free(src);
    if (rc != LUA_OK) {
        snprintf(err, errlen, "%s", lua_tostring(L, -1));
        lua_pop(L, 1);
        return false;
    }
    /* A sys.exit() or sys.off() in the top-level code or setup() leaves
       from there: it has started, and the first tick stops it. */
    if (pcall_wd(0, 1, err, errlen)) {
        s_returned_table = lua_istable(L, -1);
        lua_pop(L, 1);
        /* Looking up a global can run a metamethod on _G, so even that is protected. */
        lua_pushcfunction(L, call_setup);
        if (!pcall_wd(0, 0, err, errlen) && !s_call_left) return false;
        if (gfx_dirty()) gfx_flush();
    } else if (!s_call_left) {
        return false;
    }

    s_last_tick = hw_millis();
    s_next_due = s_last_tick;
    s_started = s_last_tick;
    s_noticed = false;
    s_running = true;
    ESP_LOGI(TAG, "running %s (heap %zu B)", name, s_heap_used);
    return true;
}

bool script_run(const char *name, char *err, size_t errlen)
{
    screen_error_clear();
    if (run_inner(name, err, errlen)) return true;
    /* The top-level code or setup() may have driven pins, opened sockets or
       queued a Bluetooth connection before failing: let go of all of it. */
    release_script_resources();
    ESP_LOGW(TAG, "%s failed: %s", name, err);
    screen_error(name, err);           /* every failure path ends up on the display */
    return false;
}

const char *script_last(void) { return s_current; }

void script_stop(void)
{
    if (s_running) release_script_resources();
    s_running = false;
}

int script_leave(lua_State *Ls, bool exit)
{
    if (exit) s_exit_requested = true;
    s_halted = s_leaving = true;
    lua_sethook(Ls, wd_hook, LUA_MASKCOUNT, 1);
    lua_pushliteral(Ls, LEAVE_MSG);
    return lua_error(Ls);
}

bool script_running(void) { return s_running; }
const char *script_current(void) { return s_running ? s_current : ""; }

/* A script that leaves the screen black looks like a hang. If it hasn't drawn
   anything, explain what's going on instead. */
static void check_nothing_drawn(bool alive, uint32_t now)
{
    if (s_noticed || lua_api_drew()) return;
    char title[VFS_MAX_NAME + 16], msg[300];
    if (!alive) {
        snprintf(title, sizeof title, "%s finished", s_current);
        if (s_returned_table) {
            char mod[VFS_MAX_NAME];
            const char *slash = strchr(s_current, '/');
            snprintf(mod, sizeof mod, "%s", slash ? slash + 1 : s_current);
            char *dot = strstr(mod, ".lua");
            if (dot) *dot = 0;
            snprintf(msg, sizeof msg,
                     "It returns a table, so it looks like a module: it is meant to be loaded "
                     "with require(\"%s\") by another script, usually main.lua. Run that one instead.",
                     mod);
        } else {
            snprintf(msg, sizeof msg,
                     "It ran without errors, but drew nothing and left no loop(), timer or task "
                     "running, so there is nothing more to see.");
        }
    } else if (now - s_started >= NOTHING_DRAWN_MS) {
        snprintf(title, sizeof title, "%s is running", s_current);
        snprintf(msg, sizeof msg,
                 "It hasn't drawn anything yet. The screen changes as soon as it does.");
    } else {
        return;
    }
    s_noticed = true;
    screen_notice(title, msg);
    char line[sizeof title + sizeof msg + 2];
    snprintf(line, sizeof line, "%s: %s", title, msg);
    proto_log(line);
}

uint32_t script_next_due_ms(void)
{
    if (!s_running || !L) return UINT32_MAX;
    int32_t left = (int32_t)(s_next_due - hw_millis());
    return left > 0 ? (uint32_t)left : 0;
}

/* After sys.exit(): back to the menu. After sys.off() the app waits,
   running nothing, until power_poll() stops it and turns the device off. */
static void halted(void)
{
    if (!s_exit_requested) return;
    script_stop();
    proto_eventf("stopped %s (exit)", s_current);
}

void script_tick(void)
{
    if (!s_running || !L) return;
    if (s_halted) { halted(); return; }
    uint32_t now = hw_millis();
    if ((int32_t)(now - s_next_due) < 0) return;
    s_next_due += s_period_ms;
    if ((int32_t)(now - s_next_due) > (int32_t)s_period_ms) s_next_due = now + s_period_ms; /* fell behind */

    uint32_t dt = now - s_last_tick;
    s_last_tick = now;

    lua_rawgeti(L, LUA_REGISTRYINDEX, s_tick_ref);
    lua_pushinteger(L, (lua_Integer)hw_millis64());   /* sys.millis(): never wraps */
    lua_pushinteger(L, (lua_Integer)dt);
    char err[200];
    /* The frame sees one moment of the touch: its events, touch.button()
       and touch.read() agree, and don't change under it. */
    buttons_frame_begin();
    bool ok = pcall_wd(2, 1, err, sizeof err);
    buttons_frame_end();
    if (ok) {
        bool alive = lua_toboolean(L, -1);
        lua_pop(L, 1);
        if (s_halted) { halted(); return; }      /* (left with nothing after the pcall that caught it) */
        check_nothing_drawn(alive, now);
        buttons_sample();        /* between Lua and the flush too */
    } else if (s_call_left) {
        halted();
        return;
    } else {
        s_running = false;
        release_script_resources();
        proto_eventf("error %s", err);
        screen_error(s_current, err);
        ESP_LOGW(TAG, "script stopped: %s", err);
    }
    if (gfx_dirty()) gfx_flush();
}

bool script_eval(const char *code, char *out, size_t outlen)
{
    if (!L) script_init();
    if (!L) { snprintf(out, outlen, "no VM"); return false; }
    int base = lua_gettop(L);

    /* Like the standalone REPL: try as an expression first, then as a statement. */
    size_t cl = strlen(code);
    char *expr = malloc(cl + 8);
    if (!expr) { snprintf(out, outlen, "out of memory"); return false; }
    memcpy(expr, "return ", 7);
    memcpy(expr + 7, code, cl + 1);
    int rc = luaL_loadbufferx(L, expr, cl + 7, "=eval", "t");
    free(expr);
    if (rc != LUA_OK) {
        lua_pop(L, 1);
        rc = luaL_loadbufferx(L, code, cl, "=eval", "t");
    }
    if (rc != LUA_OK) {
        snprintf(out, outlen, "%s", lua_tostring(L, -1));
        lua_settop(L, base);
        return false;
    }
    if (!pcall_wd(0, LUA_MULTRET, out, outlen)) {
        lua_settop(L, base);
        return s_call_left;              /* sys.exit()/sys.off(): done, the next tick stops the app */
    }
    /* The results as text: __tostring can fail, so that's protected too. */
    int nres = lua_gettop(L) - base;
    lua_pushcfunction(L, join_results);
    lua_insert(L, base + 1);
    bool ok = pcall_wd(nres, 1, out, outlen);
    if (ok) snprintf(out, outlen, "%s", lua_tostring(L, -1));
    lua_settop(L, base);
    if (!ok) return false;
    if (gfx_dirty()) gfx_flush();
    return true;
}
