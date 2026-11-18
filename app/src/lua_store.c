/*
 * Lua `store` module: small values an app keeps across runs and reboots
 * (settings, a chosen room, a counter). Each app has its own; another app
 * can't see them.
 *
 *   store.get(key) -> value | nil
 *   store.set(key, value | nil)      string, number or boolean; nil deletes
 *   store.keys() -> { key, ... }     sorted
 *   store.clear()                    delete all of this app's values
 *
 * All values of an app live in one flash file (vfs_data_*), rewritten whole
 * on every change, so set() is meant for occasional changes, not every frame.
 * A set() that changes nothing doesn't write, and vfs spaces writes at least
 * a second apart. The flash write's time doesn't count against the script's
 * watchdog (up to SAVE_MS).
 *
 * File format: "LBS1", then per entry: key length (1 byte), key, type
 * ('s' string, 'i' integer, 'f' float, 'b' boolean), value length (4 bytes,
 * little-endian), value. Integers and floats are 8 bytes, little-endian.
 */
#include "lua_store.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "app_config.h"
#include "hw.h"
#include "lauxlib.h"
#include "script.h"
#include "vfs.h"

#define MAGIC   "LBS1"
#define SAVE_MS 3000         /* the most a write may take of the watchdog's time */

typedef struct {
    const char *key;
    size_t klen;
    char type;
    const uint8_t *val;
    size_t vlen;
} entry_t;

static const char *app_of(lua_State *L, char *buf, size_t cap)
{
    script_app(buf, cap);
    if (!*buf) luaL_error(L, "store belongs to an app: run a script of an app first");
    return buf;
}

static const char *key_arg(lua_State *L, int idx, size_t *len)
{
    const char *k = luaL_checklstring(L, idx, len);
    bool ok = *len >= 1 && *len <= STORE_KEY_MAX;
    for (size_t i = 0; ok && i < *len; i++)
        ok = isalnum((unsigned char)k[i]) || k[i] == '_' || k[i] == '.' || k[i] == '-';
    if (!ok) luaL_argerror(L, idx, "keys are 1..32 characters: A-Z a-z 0-9 _ . -");
    return k;
}

/* The app's data file in a Lua-owned buffer (left on the stack), so a Lua
   error later can't leak it. Returns its length; 0 if there is none. */
static size_t load(lua_State *L, const char *app, const uint8_t **out)
{
    long n = vfs_data_size(app);
    if (n < 0 || n > STORE_MAX_BYTES) n = 0;            /* none, or not ours: start over */
    uint8_t *buf = lua_newuserdatauv(L, n ? (size_t)n : 1, 0);
    long got = n ? vfs_data_read(app, buf, (size_t)n) : 0;
    if (got != n || (n && (n < 4 || memcmp(buf, MAGIC, 4)))) got = 0;
    *out = buf;
    return (size_t)got;
}

/* Next entry from *pos; false at the end or on a damaged entry (the rest is
   dropped). */
static bool next(const uint8_t *buf, size_t len, size_t *pos, entry_t *e)
{
    size_t p = *pos;
    if (p == 0) p = 4;                                  /* after the magic */
    if (p + 1 > len) return false;
    e->klen = buf[p++];
    if (!e->klen || p + e->klen + 5 > len) return false;
    e->key = (const char *)buf + p;
    p += e->klen;
    e->type = (char)buf[p++];
    e->vlen = (size_t)buf[p] | (size_t)buf[p + 1] << 8 | (size_t)buf[p + 2] << 16 | (size_t)buf[p + 3] << 24;
    p += 4;
    if (e->vlen > len - p) return false;
    e->val = buf + p;
    *pos = p + e->vlen;
    return true;
}

static bool find(const uint8_t *buf, size_t len, const char *key, size_t klen, entry_t *e)
{
    size_t pos = 0;
    while (next(buf, len, &pos, e))
        if (e->klen == klen && !memcmp(e->key, key, klen)) return true;
    return false;
}

static void put_u64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static uint64_t get_u64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)p[i] << (8 * i);
    return v;
}

static int l_get(lua_State *L)
{
    size_t klen;
    const char *key = key_arg(L, 1, &klen);
    char app[VFS_APP_NAME_MAX];
    const uint8_t *buf;
    size_t len = load(L, app_of(L, app, sizeof app), &buf);
    entry_t e;
    if (!find(buf, len, key, klen, &e)) { lua_pushnil(L); return 1; }
    if (e.type == 's') lua_pushlstring(L, (const char *)e.val, e.vlen);
    else if (e.type == 'i' && e.vlen == 8) lua_pushinteger(L, (lua_Integer)get_u64(e.val));
    else if (e.type == 'f' && e.vlen == 8) {
        uint64_t u = get_u64(e.val);
        double d;
        memcpy(&d, &u, sizeof d);
        lua_pushnumber(L, (lua_Number)d);
    } else if (e.type == 'b' && e.vlen == 1) lua_pushboolean(L, e.val[0]);
    else lua_pushnil(L);
    return 1;
}

static int l_set(lua_State *L)
{
    size_t klen;
    const char *key = key_arg(L, 1, &klen);
    char type = 0;
    uint8_t num[8];
    const uint8_t *val = NULL;
    size_t vlen = 0;
    switch (lua_type(L, 2)) {
    case LUA_TNONE: case LUA_TNIL: break;
    case LUA_TSTRING: type = 's'; val = (const uint8_t *)lua_tolstring(L, 2, &vlen); break;
    case LUA_TBOOLEAN: type = 'b'; num[0] = (uint8_t)lua_toboolean(L, 2); val = num; vlen = 1; break;
    case LUA_TNUMBER:
        if (lua_isinteger(L, 2)) { type = 'i'; put_u64(num, (uint64_t)lua_tointeger(L, 2)); }
        else {
            type = 'f';
            double d = (double)lua_tonumber(L, 2);
            uint64_t u;
            memcpy(&u, &d, sizeof u);
            put_u64(num, u);
        }
        val = num; vlen = 8;
        break;
    default:
        return luaL_argerror(L, 2, "store keeps strings, numbers and booleans (json.encode a table)");
    }

    char app[VFS_APP_NAME_MAX];
    app_of(L, app, sizeof app);
    const uint8_t *old;
    size_t olen = load(L, app, &old);
    entry_t e;
    bool had = find(old, olen, key, klen, &e);
    if (had && type && e.type == type && e.vlen == vlen && !memcmp(e.val, val, vlen)) return 0;  /* unchanged */
    if (!had && !type) return 0;                                                               /* nothing to delete */

    /* The new file: every other entry, then this one. The entry it replaces
       doesn't count against the limit. */
    size_t cap = (olen ? olen : 4) - (had ? 1 + e.klen + 5 + e.vlen : 0) + (type ? 1 + klen + 5 + vlen : 0);
    if (type && cap > STORE_MAX_BYTES)
        return luaL_error(L, "store full (max %d bytes per app)", STORE_MAX_BYTES);
    uint8_t *out = lua_newuserdatauv(L, cap, 0);
    memcpy(out, MAGIC, 4);
    size_t o = 4, pos = 0;
    while (next(old, olen, &pos, &e)) {
        if (e.klen == klen && !memcmp(e.key, key, klen)) continue;
        size_t n = 1 + e.klen + 5 + e.vlen;
        memcpy(out + o, e.key - 1, n);                   /* the entry as stored */
        o += n;
    }
    if (type) {
        out[o++] = (uint8_t)klen;
        memcpy(out + o, key, klen); o += klen;
        out[o++] = (uint8_t)type;
        for (int i = 0; i < 4; i++) out[o++] = (uint8_t)(vlen >> (8 * i));
        memcpy(out + o, val, vlen); o += vlen;
    }
    uint32_t t0 = hw_millis();
    esp_err_t err = vfs_data_save(app, out, o > 4 ? o : 0);
    script_watchdog_credit(L, t0, SAVE_MS);
    if (err != ESP_OK) return luaL_error(L, "saving failed (%s)", esp_err_to_name(err));
    return 0;
}

static int cmp_entry(const void *a, const void *b)
{
    const entry_t *x = a, *y = b;
    size_t n = x->klen < y->klen ? x->klen : y->klen;
    int c = memcmp(x->key, y->key, n);
    return c ? c : (x->klen > y->klen) - (x->klen < y->klen);
}

static int l_keys(lua_State *L)
{
    char app[VFS_APP_NAME_MAX];
    const uint8_t *buf;
    size_t len = load(L, app_of(L, app, sizeof app), &buf);
    size_t n = 0, pos = 0;
    entry_t e;
    while (next(buf, len, &pos, &e)) n++;
    entry_t *all = lua_newuserdatauv(L, (n ? n : 1) * sizeof *all, 0);
    for (size_t i = 0, p = 0; i < n; i++) next(buf, len, &p, &all[i]);
    qsort(all, n, sizeof *all, cmp_entry);
    lua_createtable(L, (int)n, 0);
    for (size_t i = 0; i < n; i++) {
        lua_pushlstring(L, all[i].key, all[i].klen);
        lua_rawseti(L, -2, (lua_Integer)i + 1);
    }
    return 1;
}

static int l_clear(lua_State *L)
{
    char app[VFS_APP_NAME_MAX];
    app_of(L, app, sizeof app);
    uint32_t t0 = hw_millis();
    vfs_data_clear(app);
    script_watchdog_credit(L, t0, SAVE_MS);
    return 0;
}

void lua_store_register(lua_State *L)
{
    static const luaL_Reg fns[] = {
        { "get", l_get }, { "set", l_set }, { "keys", l_keys }, { "clear", l_clear }, { NULL, NULL },
    };
    luaL_newlib(L, fns);
    lua_setglobal(L, "store");
}
