/*
 * Lua `ble` module: Bluetooth LE central (see ble.c). The C side is
 * non-blocking; the prelude builds the waiting helpers on it (ble.scan with
 * a callback, c:read / c:write / c:subscribe inside a task).
 *
 *   ble._scan(active) -> true | nil, err      start scanning
 *   ble._stop_scan()
 *   ble._adv()       -> adv | nil             next device heard from:
 *       { addr, rssi, name, mfg = { [company] = data }, svc = { [uuid] = data },
 *         uuids = { uuid, ... }, tx, raw }
 *   ble.connect(addr) -> conn | nil, err
 *   ble.uuid(s)      -> canonical UUID text ("2A19", "0x2a19" -> "2a19") | nil
 *   conn:status()    -> "connecting" | "connected" | "closed", err
 *   conn:addr(), conn:close()
 *   conn:services()  -> { uuid, ... }
 *   conn:characteristics([svc]) -> { { svc, uuid, read, write, write_nr, notify, indicate }, ... }
 *   conn:_read(svc, chr) / conn:_write(svc, chr, data) / conn:_subscribe(svc, chr, on)
 *                    -> token | nil, err
 *   conn:_poll(token) -> "pending" | "done", data | "error", err
 *   conn:_notified() -> svc, chr, data | nil
 *   ble._methods     the connection methods (the prelude adds to them)
 */
#include "lua_ble.h"

#include <stdio.h>
#include <string.h>

#include "app_config.h"
#include "ble.h"
#include "script.h"
#include "lauxlib.h"

#define MT_CONN "luaboard.ble"

typedef struct {
    int id;
    bool closed;
    char addr[18];
} lconn_t;

static int push_err(lua_State *L, const char *msg)
{
    lua_pushnil(L);
    lua_pushstring(L, msg);
    return 2;
}

static void push_uuid(lua_State *L, const ble_uuid_any_t *u)
{
    char s[BLE_UUID_STR_LEN];
    ble_uuid_format(u, s);
    lua_pushstring(L, s);
}

/* ------------------------------------------------------------- scanning */

static int l_scan(lua_State *L)
{
    char err[96];
    if (!ble_scan_start(lua_toboolean(L, 1), err, sizeof err)) return push_err(L, err);
    lua_pushboolean(L, 1);
    return 1;
}

static int l_stop_scan(lua_State *L)
{
    ble_scan_stop();
    return 0;
}

/* t[key] = data, for the svc / mfg tables (created on first use). */
static void set_in(lua_State *L, int adv, const char *field, int key_is_int, lua_Integer ikey,
                   const char *skey, const uint8_t *d, size_t n)
{
    lua_getfield(L, adv, field);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setfield(L, adv, field);
    }
    if (key_is_int) lua_pushinteger(L, ikey);
    else lua_pushstring(L, skey);
    lua_pushlstring(L, (const char *)d, n);
    lua_settable(L, -3);
    lua_pop(L, 1);
}

static void add_uuid(lua_State *L, int adv, const uint8_t *d, size_t size)
{
    ble_uuid_any_t u;
    if (ble_uuid_init_from_buf(&u, d, size) != 0) return;
    lua_getfield(L, adv, "uuids");
    push_uuid(L, &u);
    lua_rawseti(L, -2, (lua_Integer)lua_rawlen(L, -2) + 1);
    lua_pop(L, 1);
}

/* The advertising data's AD structures: [len][type][data...]. */
static void parse_ad(lua_State *L, int adv, const uint8_t *ad, size_t len)
{
    bool complete_name = false;
    size_t i = 0;
    while (i + 1 < len) {
        size_t n = ad[i];
        if (n == 0 || i + 1 + n > len) break;
        uint8_t type = ad[i + 1];
        const uint8_t *d = ad + i + 2;
        size_t dl = n - 1;
        switch (type) {
        case 0x02: case 0x03:                          /* 16-bit service UUIDs */
            for (size_t k = 0; k + 2 <= dl; k += 2) add_uuid(L, adv, d + k, 2);
            break;
        case 0x04: case 0x05:
            for (size_t k = 0; k + 4 <= dl; k += 4) add_uuid(L, adv, d + k, 4);
            break;
        case 0x06: case 0x07:
            for (size_t k = 0; k + 16 <= dl; k += 16) add_uuid(L, adv, d + k, 16);
            break;
        case 0x08: case 0x09:                          /* shortened / complete name */
            if (type == 0x09 || !complete_name) {
                lua_pushlstring(L, (const char *)d, dl);
                lua_setfield(L, adv, "name");
                complete_name = complete_name || type == 0x09;
            }
            break;
        case 0x0a:
            if (dl >= 1) { lua_pushinteger(L, (int8_t)d[0]); lua_setfield(L, adv, "tx"); }
            break;
        case 0x16: case 0x20: case 0x21: {              /* service data: 16, 32, 128-bit UUID */
            size_t us = type == 0x16 ? 2 : type == 0x20 ? 4 : 16;
            ble_uuid_any_t u;
            if (dl >= us && ble_uuid_init_from_buf(&u, d, us) == 0) {
                char s[BLE_UUID_STR_LEN];
                ble_uuid_format(&u, s);
                set_in(L, adv, "svc", 0, 0, s, d + us, dl - us);
            }
            break;
        }
        case 0xff:                                     /* manufacturer data: company ID first */
            if (dl >= 2) set_in(L, adv, "mfg", 1, d[0] | d[1] << 8, NULL, d + 2, dl - 2);
            break;
        }
        i += 1 + n;
    }
}

static int l_adv(lua_State *L)
{
    ble_adv_t a;
    if (!ble_adv_next(&a)) return 0;
    lua_createtable(L, 0, 6);
    int t = lua_gettop(L);
    char s[18];
    ble_addr_format(&a.addr, s);
    lua_pushstring(L, s);
    lua_setfield(L, t, "addr");
    lua_pushinteger(L, a.rssi);
    lua_setfield(L, t, "rssi");
    lua_newtable(L);
    lua_setfield(L, t, "uuids");
    lua_pushlstring(L, (const char *)a.ad, a.ad_len);
    lua_setfield(L, t, "raw");
    parse_ad(L, t, a.ad, a.ad_len);
    return 1;
}

/* ble.uuid(s) -> the canonical text of a UUID, nil if malformed. */
static int l_uuid(lua_State *L)
{
    ble_uuid_any_t u;
    if (ble_uuid_parse(luaL_checkstring(L, 1), &u)) push_uuid(L, &u);
    else lua_pushnil(L);
    return 1;
}

/* ---------------------------------------------------------- connections */

static lconn_t *check_conn(lua_State *L) { return luaL_checkudata(L, 1, MT_CONN); }

static int l_connect(lua_State *L)
{
    const char *s = luaL_checkstring(L, 1);
    ble_addr_t addr;
    if (!ble_addr_parse(s, &addr)) return luaL_error(L, "ble.connect: bad address '%s' (like \"c7:18:2c:93:8f:cc\")", s);
    lconn_t *c = lua_newuserdatauv(L, sizeof *c, 0);
    c->id = -1;
    c->closed = true;
    ble_addr_format(&addr, c->addr);
    luaL_setmetatable(L, MT_CONN);
    char err[96], app[VFS_APP_NAME_MAX];
    script_app(app, sizeof app);
    int id = ble_connect(&addr, app, err, sizeof err);
    if (id < 0) return push_err(L, err);
    c->id = id;
    c->closed = false;
    return 1;
}

static int m_status(lua_State *L)
{
    lconn_t *c = check_conn(L);
    char err[96] = "closed";
    ble_state_t st = c->closed ? BLE_CLOSED : ble_state(c->id, err, sizeof err);
    lua_pushstring(L, st == BLE_CONNECTED ? "connected" : st == BLE_CONNECTING ? "connecting" : "closed");
    if (st != BLE_CLOSED) return 1;
    lua_pushstring(L, err);
    return 2;
}

static int m_addr(lua_State *L)
{
    lua_pushstring(L, check_conn(L)->addr);
    return 1;
}

static int m_close(lua_State *L)
{
    lconn_t *c = luaL_checkudata(L, 1, MT_CONN);
    if (!c->closed) ble_close(c->id);
    c->closed = true;
    return 0;
}

static int m_services(lua_State *L)
{
    lconn_t *c = check_conn(L);
    lua_newtable(L);
    int n = c->closed ? 0 : ble_chr_count(c->id), k = 0;
    ble_uuid_any_t last;
    for (int i = 0; i < n; i++) {
        ble_chr_info_t info;
        if (!ble_chr_info(c->id, i, &info)) break;
        if (k && !ble_uuid_cmp(&info.svc.u, &last.u)) continue;
        last = info.svc;
        push_uuid(L, &info.svc);
        lua_rawseti(L, -2, ++k);
    }
    return 1;
}

static int m_characteristics(lua_State *L)
{
    lconn_t *c = check_conn(L);
    ble_uuid_any_t want;
    bool filter = !lua_isnoneornil(L, 2);
    if (filter && !ble_uuid_parse(luaL_checkstring(L, 2), &want))
        return luaL_error(L, "bad UUID '%s'", lua_tostring(L, 2));
    lua_newtable(L);
    int n = c->closed ? 0 : ble_chr_count(c->id), k = 0;
    for (int i = 0; i < n; i++) {
        ble_chr_info_t info;
        if (!ble_chr_info(c->id, i, &info)) break;
        if (filter && ble_uuid_cmp(&info.svc.u, &want.u)) continue;
        lua_createtable(L, 0, 7);
        push_uuid(L, &info.svc);
        lua_setfield(L, -2, "svc");
        push_uuid(L, &info.uuid);
        lua_setfield(L, -2, "uuid");
        static const struct { const char *name; uint8_t bit; } flags[] = {
            { "read", BLE_GATT_CHR_PROP_READ }, { "write", BLE_GATT_CHR_PROP_WRITE },
            { "write_nr", BLE_GATT_CHR_PROP_WRITE_NO_RSP }, { "notify", BLE_GATT_CHR_PROP_NOTIFY },
            { "indicate", BLE_GATT_CHR_PROP_INDICATE },
        };
        for (size_t f = 0; f < sizeof flags / sizeof flags[0]; f++) {
            lua_pushboolean(L, (info.props & flags[f].bit) != 0);
            lua_setfield(L, -2, flags[f].name);
        }
        lua_rawseti(L, -2, ++k);
    }
    return 1;
}

/* The characteristic (svc, chr) named by args 2 and 3; pushes nil, err and
   returns -1 if there's no such one. */
static int find_chr(lua_State *L, lconn_t *c, int *out)
{
    const char *ss = luaL_checkstring(L, 2), *cs = luaL_checkstring(L, 3);
    ble_uuid_any_t svc, chr;
    if (!ble_uuid_parse(ss, &svc)) return luaL_error(L, "bad service UUID '%s'", ss);
    if (!ble_uuid_parse(cs, &chr)) return luaL_error(L, "bad characteristic UUID '%s'", cs);
    char err[96];
    if (c->closed) return push_err(L, "closed");
    if (ble_state(c->id, err, sizeof err) != BLE_CONNECTED) return push_err(L, ble_state(c->id, err, sizeof err) == BLE_CLOSED ? err : "still connecting");
    int i = ble_chr_find(c->id, &svc, &chr);
    if (i < 0) {
        char m[160];
        snprintf(m, sizeof m, "the device has no characteristic %s in service %s", cs, ss);
        return push_err(L, m);
    }
    *out = i;
    return 0;
}

static int push_token(lua_State *L, int token, const char *err)
{
    if (!token) return push_err(L, err);
    lua_pushinteger(L, token);
    return 1;
}

static int m_read(lua_State *L)
{
    lconn_t *c = check_conn(L);
    int chr, r = find_chr(L, c, &chr);
    if (r) return r;
    char err[96];
    return push_token(L, ble_read(c->id, chr, err, sizeof err), err);
}

static int m_write(lua_State *L)
{
    lconn_t *c = check_conn(L);
    size_t len;
    const char *data = luaL_checklstring(L, 4, &len);
    int chr, r = find_chr(L, c, &chr);
    if (r) return r;
    ble_chr_info_t info;
    ble_chr_info(c->id, chr, &info);
    /* With a response if the characteristic takes one, else without. */
    bool response = (info.props & BLE_GATT_CHR_PROP_WRITE) || !(info.props & BLE_GATT_CHR_PROP_WRITE_NO_RSP);
    char err[96];
    return push_token(L, ble_write(c->id, chr, data, len, response, err, sizeof err), err);
}

static int m_subscribe(lua_State *L)
{
    lconn_t *c = check_conn(L);
    bool on = lua_isnone(L, 4) || lua_toboolean(L, 4);
    int chr, r = find_chr(L, c, &chr);
    if (r) return r;
    char err[96];
    return push_token(L, ble_subscribe(c->id, chr, on, err, sizeof err), err);
}

static int m_poll(lua_State *L)
{
    check_conn(L);
    int token = (int)luaL_checkinteger(L, 2);
    static uint8_t buf[BLE_VALUE_MAX];
    size_t len = 0;
    char err[96];
    int r = ble_result(token, buf, sizeof buf, &len, err, sizeof err);
    if (r == 0) { lua_pushliteral(L, "pending"); return 1; }
    if (r > 0) { lua_pushliteral(L, "done"); lua_pushlstring(L, (const char *)buf, len); return 2; }
    lua_pushliteral(L, "error");
    lua_pushstring(L, err);
    return 2;
}

static int m_notified(lua_State *L)
{
    lconn_t *c = check_conn(L);
    if (c->closed) return 0;
    static uint8_t buf[BLE_VALUE_MAX];
    size_t len;
    int chr;
    if (!ble_notified(c->id, &chr, buf, sizeof buf, &len)) return 0;
    ble_chr_info_t info;
    if (!ble_chr_info(c->id, chr, &info)) return 0;
    push_uuid(L, &info.svc);
    push_uuid(L, &info.uuid);
    lua_pushlstring(L, (const char *)buf, len);
    return 3;
}

static int m_tostring(lua_State *L)
{
    lconn_t *c = check_conn(L);
    char err[96];
    ble_state_t st = c->closed ? BLE_CLOSED : ble_state(c->id, err, sizeof err);
    lua_pushfstring(L, "ble(%s, %s)", c->addr,
                    st == BLE_CONNECTED ? "connected" : st == BLE_CONNECTING ? "connecting" : "closed");
    return 1;
}

void lua_ble_register(lua_State *L)
{
    static const luaL_Reg methods[] = {
        { "status", m_status }, { "addr", m_addr }, { "close", m_close },
        { "services", m_services }, { "characteristics", m_characteristics },
        { "_read", m_read }, { "_write", m_write }, { "_subscribe", m_subscribe },
        { "_poll", m_poll }, { "_notified", m_notified },
        { NULL, NULL },
    };
    luaL_newmetatable(L, MT_CONN);
    luaL_newlib(L, methods);
    int idx = lua_gettop(L);
    lua_pushvalue(L, idx);
    lua_setfield(L, -3, "__index");
    lua_pushcfunction(L, m_close);
    lua_setfield(L, -3, "__gc");
    lua_pushcfunction(L, m_close);
    lua_setfield(L, -3, "__close");
    lua_pushcfunction(L, m_tostring);
    lua_setfield(L, -3, "__tostring");
    lua_pushboolean(L, 0);
    lua_setfield(L, -3, "__metatable");   /* see lua_net.c: keep __gc out of reach */

    static const luaL_Reg funcs[] = {
        { "_scan", l_scan }, { "_stop_scan", l_stop_scan }, { "_adv", l_adv },
        { "connect", l_connect }, { "uuid", l_uuid }, { NULL, NULL },
    };
    luaL_newlib(L, funcs);
    lua_pushvalue(L, idx);
    lua_setfield(L, -2, "_methods");
    lua_setglobal(L, "ble");
    lua_pop(L, 2);                        /* methods, metatable */
}
