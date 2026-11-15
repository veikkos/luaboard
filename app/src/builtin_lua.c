/*
 * Lua sources built into the firmware. The .lua files in app/lua/ are the
 * source of truth; the build turns them into byte arrays in
 * builtin_lua_data.h (cmake/embed_files.cmake), the same for the device and
 * the simulator. A script file uploaded under the same name overrides a
 * built-in library.
 */
#include "builtin_lua.h"

#include <string.h>

#include "builtin_lua_data.h"

static const struct { const char *name; const char *src; size_t len; } s_builtins[] = {
    { "prelude", embed_prelude_lua, sizeof embed_prelude_lua - 1 },
    { "mqtt.lua", embed_mqtt_lua, sizeof embed_mqtt_lua - 1 },
    { "http.lua", embed_http_lua, sizeof embed_http_lua - 1 },
    { "oauth.lua", embed_oauth_lua, sizeof embed_oauth_lua - 1 },
    { "json.lua", embed_json_lua, sizeof embed_json_lua - 1 },
};

const char *builtin_lua(const char *name, size_t *len)
{
    for (size_t i = 0; i < sizeof s_builtins / sizeof s_builtins[0]; i++) {
        if (!strcmp(s_builtins[i].name, name)) {
            if (len) *len = s_builtins[i].len;
            return s_builtins[i].src;
        }
    }
    return NULL;
}
