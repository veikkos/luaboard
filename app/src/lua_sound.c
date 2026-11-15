/*
 * Lua `sound`: tones and WAV clips on the speaker, queued and played in
 * order while the script goes on (sound.c). Loudness is the device's Volume
 * setting times each sound's own volume, so a script can't be louder than
 * the user allowed. Everything stops when the script ends.
 */
#include "lua_sound.h"

#include "app_config.h"
#include "lauxlib.h"
#include "settings.h"
#include "sound.h"

static int opt_volume(lua_State *L, int idx)
{
    lua_Integer v = luaL_optinteger(L, idx, 100);
    luaL_argcheck(L, v >= 0 && v <= 100, idx, "volume must be 0..100");
    return (int)v;
}

/* A result: true, or nil and why not. */
static int result(lua_State *L, const char *err)
{
    if (!err) {
        lua_pushboolean(L, 1);
        return 1;
    }
    lua_pushnil(L);
    lua_pushstring(L, err);
    return 2;
}

static int l_tone(lua_State *L)
{
    lua_Integer hz = luaL_checkinteger(L, 1), ms = luaL_checkinteger(L, 2);
    luaL_argcheck(L, hz == 0 || (hz >= 20 && hz <= 20000), 1, "frequency must be 20..20000 Hz, or 0 for a rest");
    luaL_argcheck(L, ms >= 1 && ms <= SOUND_TONE_MAX_MS, 2, "length must be 1..10000 ms");
    return result(L, sound_tone((int)hz, (int)ms, opt_volume(L, 3)));
}

static int l_play(lua_State *L)
{
    size_t len;
    const char *wav = luaL_checklstring(L, 1, &len);
    return result(L, sound_wav(wav, len, opt_volume(L, 2)));
}

static int l_stop(lua_State *L)
{
    (void)L;
    sound_stop();
    return 0;
}

static int l_playing(lua_State *L)
{
    lua_pushboolean(L, sound_playing());
    return 1;
}

static int l_volume(lua_State *L)
{
    lua_pushinteger(L, settings_volume());
    return 1;
}

static const luaL_Reg funcs[] = {
    { "tone", l_tone }, { "play", l_play }, { "stop", l_stop }, { "playing", l_playing }, { "volume", l_volume },
    { NULL, NULL },
};

void lua_sound_register(lua_State *L)
{
    luaL_newlib(L, funcs);
    lua_setglobal(L, "sound");
}
