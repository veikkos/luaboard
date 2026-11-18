/*
 * Lua modules for web services:
 *
 *   secrets.get(name) -> value | nil       values kept in NVS, not in the scripts;
 *   secrets.set(name, value | nil)         nil deletes. Only the running app's own
 *   secrets.keys() -> { name, ... }        secrets: other apps' can't be reached. Sorted.
 *
 *   crypto.sha256(data) -> 32 raw bytes
 *   crypto.hmac_sha256(key, data) -> 32 raw bytes
 *   crypto.random(n) -> n random bytes (hardware RNG)
 *   crypto.hex(data) -> lowercase hex
 *   crypto.base64(data, [url]) -> text (url = true: URL-safe, no padding)
 *   crypto.unbase64(text) -> data | nil
 *
 *   browser._start(url, redirect, title) -> id     (browser.login() in the
 *   browser._poll(id) -> state, result               prelude wraps these)
 *   browser._cancel(id)
 */
#include "lua_web.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_config.h"
#include "browser.h"
#include "esp_random.h"
#include "hex.h"
#include "lauxlib.h"
#include "lua_api.h"
#include "nvs.h"
#include "script.h"
#include "secrets.h"
#include "sha256.h"

/* ------------------------------------------------------------- secrets */

static const char *name_arg(lua_State *L, int idx)
{
    const char *n = luaL_checkstring(L, idx);
    if (!secrets_valid_name(n)) luaL_argerror(L, idx, "secret names are 1..15 characters: A-Z a-z 0-9 _ . -");
    return n;
}

/* Secrets belong to the app of the running script: other apps' are out of reach. */
static const char *app_of(lua_State *L, char *buf, size_t cap)
{
    script_app(buf, cap);
    if (!*buf) luaL_error(L, "secrets belong to an app: run a script of an app first");
    return buf;
}

static int l_secret_get(lua_State *L)
{
    size_t len;
    char app[VFS_APP_NAME_MAX];
    const char *n = name_arg(L, 1);
    app_of(L, app, sizeof app);
    /* Room in Lua first: nothing may throw while the copy is held. */
    luaL_Buffer b;
    char *dst = luaL_buffinitsize(L, &b, SECRET_MAX_LEN);
    char *v = secrets_get(app, n, &len);
    if (!v) { lua_pushnil(L); return 1; }
    if (len > SECRET_MAX_LEN) len = SECRET_MAX_LEN;
    memcpy(dst, v, len);
    free(v);
    luaL_pushresultsize(&b, len);
    return 1;
}

static int l_secret_set(lua_State *L)
{
    const char *n = name_arg(L, 1);
    size_t len = 0;
    const char *v = lua_isnoneornil(L, 2) ? NULL : luaL_checklstring(L, 2, &len);
    char app[VFS_APP_NAME_MAX];
    esp_err_t err = secrets_set(app_of(L, app, sizeof app), n, v, len);
    if (err == ESP_ERR_INVALID_SIZE) return luaL_error(L, "secret too long (max %d bytes)", SECRET_MAX_LEN);
    if (err == ESP_ERR_NO_MEM) return luaL_error(L, "too many secrets (max %d on the device)", SECRET_MAX_COUNT);
    if (err == ESP_ERR_NVS_NOT_ENOUGH_SPACE)
        return luaL_error(L, "no room for the secret (an app's secrets can take %d bytes, all apps' %d)",
                          SECRETS_APP_BYTES, SECRETS_TOTAL_BYTES);
    if (err != ESP_OK) return luaL_error(L, "storing the secret failed (%s)", esp_err_to_name(err));
    return 0;
}

typedef struct {
    char names[SECRET_MAX_COUNT][16];
    int n;
} names_t;

static void list_one(const char *app, const char *name, size_t len, void *ctx)
{
    (void)app; (void)len;
    names_t *nm = ctx;
    if (nm->n < SECRET_MAX_COUNT) snprintf(nm->names[nm->n++], sizeof nm->names[0], "%s", name);
}

static int cmp_name(const void *a, const void *b) { return strcmp(a, b); }

static int l_secret_keys(lua_State *L)
{
    char app[VFS_APP_NAME_MAX];
    app_of(L, app, sizeof app);
    /* Collect first, then build the table: a Lua error (out of memory) inside
       secrets_list would leak its buffer and NVS handle. */
    names_t nm = { .n = 0 };
    secrets_list(app, list_one, &nm);
    qsort(nm.names, (size_t)nm.n, sizeof nm.names[0], cmp_name);
    lua_createtable(L, nm.n, 0);
    for (int i = 0; i < nm.n; i++) {
        lua_pushstring(L, nm.names[i]);
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

/* -------------------------------------------------------------- crypto */

static int l_sha256(lua_State *L)
{
    size_t len;
    const char *d = luaL_checklstring(L, 1, &len);
    uint8_t out[32];
    lb_sha256(d, len, out);
    lua_pushlstring(L, (const char *)out, 32);
    return 1;
}

static int l_hmac(lua_State *L)
{
    size_t kl, dl;
    const char *k = luaL_checklstring(L, 1, &kl), *d = luaL_checklstring(L, 2, &dl);
    uint8_t out[32];
    lb_hmac_sha256(k, kl, d, dl, out);
    lua_pushlstring(L, (const char *)out, 32);
    return 1;
}

static int l_random(lua_State *L)
{
    lua_Integer n = luaL_checkinteger(L, 1);
    luaL_argcheck(L, n >= 0 && n <= 4096, 1, "0..4096 bytes");
    luaL_Buffer b;
    char *p = luaL_buffinitsize(L, &b, (size_t)n);
    esp_fill_random(p, (size_t)n);
    luaL_pushresultsize(&b, (size_t)n);
    return 1;
}

static int l_hex(lua_State *L)
{
    size_t len;
    const unsigned char *d = (const unsigned char *)luaL_checklstring(L, 1, &len);
    luaL_Buffer b;
    hex_encode(d, len, luaL_buffinitsize(L, &b, len * 2 + 1));
    luaL_pushresultsize(&b, len * 2);
    return 1;
}

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static const char B64URL[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

static int l_base64(lua_State *L)
{
    size_t len;
    const unsigned char *d = (const unsigned char *)luaL_checklstring(L, 1, &len);
    bool url = lua_api_optbool(L, 2, false);
    const char *tab = url ? B64URL : B64;
    luaL_Buffer b;
    luaL_buffinit(L, &b);
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = (uint32_t)d[i] << 16;
        if (i + 1 < len) v |= (uint32_t)d[i + 1] << 8;
        if (i + 2 < len) v |= d[i + 2];
        luaL_addchar(&b, tab[(v >> 18) & 63]);
        luaL_addchar(&b, tab[(v >> 12) & 63]);
        if (i + 1 < len) luaL_addchar(&b, tab[(v >> 6) & 63]); else if (!url) luaL_addchar(&b, '=');
        if (i + 2 < len) luaL_addchar(&b, tab[v & 63]); else if (!url) luaL_addchar(&b, '=');
    }
    luaL_pushresult(&b);
    return 1;
}

static int b64val(int c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
}

static int l_unbase64(lua_State *L)
{
    size_t len;
    const char *t = luaL_checklstring(L, 1, &len);
    luaL_Buffer b;
    luaL_buffinit(L, &b);
    uint32_t acc = 0;
    int bits = 0;
    for (size_t i = 0; i < len; i++) {
        int c = (unsigned char)t[i];
        if (c == '=' || c == '\n' || c == '\r' || c == ' ') continue;
        int v = b64val(c);
        if (v < 0) { lua_pushnil(L); return 1; }
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) { bits -= 8; luaL_addchar(&b, (char)((acc >> bits) & 0xFF)); }
    }
    luaL_pushresult(&b);
    return 1;
}

/* ------------------------------------------------------------- browser */

static int l_br_start(lua_State *L)
{
    const char *err = NULL;
    char app[VFS_APP_NAME_MAX];
    script_app(app, sizeof app);          /* who asks: the firmware says, not the script */
    int id = browser_start(app, luaL_checkstring(L, 1), luaL_checkstring(L, 2), luaL_optstring(L, 3, ""), &err);
    if (!id) return luaL_error(L, "browser.login: %s", err);
    lua_pushinteger(L, id);
    return 1;
}

static int l_br_poll(lua_State *L)
{
    const char *res = NULL;
    switch (browser_poll((int)luaL_checkinteger(L, 1), &res)) {
    case BROWSER_PENDING: lua_pushliteral(L, "pending"); return 1;
    case BROWSER_DONE: lua_pushliteral(L, "done"); lua_pushstring(L, res); return 2;
    case BROWSER_CANCELLED: lua_pushliteral(L, "cancelled"); return 1;
    default: lua_pushliteral(L, "cancelled"); return 1;
    }
}

static int l_br_cancel(lua_State *L)
{
    browser_cancel((int)luaL_checkinteger(L, 1));
    return 0;
}

void lua_web_register(lua_State *L)
{
    static const luaL_Reg sec[] = {
        { "get", l_secret_get }, { "set", l_secret_set }, { "keys", l_secret_keys }, { NULL, NULL },
    };
    luaL_newlib(L, sec);
    lua_setglobal(L, "secrets");

    static const luaL_Reg cr[] = {
        { "sha256", l_sha256 }, { "hmac_sha256", l_hmac }, { "random", l_random }, { "hex", l_hex },
        { "base64", l_base64 }, { "unbase64", l_unbase64 }, { NULL, NULL },
    };
    luaL_newlib(L, cr);
    lua_setglobal(L, "crypto");

    static const luaL_Reg br[] = {
        { "_start", l_br_start }, { "_poll", l_br_poll }, { "_cancel", l_br_cancel }, { NULL, NULL },
    };
    luaL_newlib(L, br);
    lua_setglobal(L, "browser");
}
