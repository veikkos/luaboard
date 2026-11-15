#include "appmeta.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_config.h"
#include "json.h"
#include "vfs.h"

_Static_assert(sizeof ((app_meta_t *)0)->version >= sizeof "1.0.0+a1b2c3d-dirty",
               "app_meta_t.version holds a dev build's version");

bool app_meta_parse(const char *text, size_t len, app_meta_t *m)
{
    json_t j;
    char key[24];
    memset(m, 0, sizeof *m);
    json_init(&j, text, len);
    while (json_member(&j, key, sizeof key)) {
        char *dst = NULL;
        size_t cap = 0;
        if (!strcmp(key, "title")) { dst = m->title; cap = sizeof m->title; }
        else if (!strcmp(key, "description")) { dst = m->description; cap = sizeof m->description; }
        else if (!strcmp(key, "version")) { dst = m->version; cap = sizeof m->version; }
        else if (!strcmp(key, "author")) { dst = m->author; cap = sizeof m->author; }
        if (!strcmp(key, "lua_api")) {
            json_t at = j;
            long v;
            if (!json_whole(&j, &v)) {                  /* maybe well-formed, but not a whole number */
                j = at;
                json_value(&j, NULL, 0);
                v = -1;
            }
            m->lua_api = v >= 1 && v <= 1000000 ? (int)v : -1;
        } else {
            json_value(&j, dst, cap);
        }
    }
    if (!j.err) return true;
    memset(m, 0, sizeof *m);
    return false;
}

bool app_meta_load(const char *app, app_meta_t *m)
{
    char path[VFS_MAX_NAME];
    memset(m, 0, sizeof *m);
    snprintf(path, sizeof path, "%.*s/%s", VFS_APP_NAME_MAX - 1, app, APP_META_FILE);
    size_t len = 0;
    uint8_t *data = vfs_load(path, &len);
    if (!data) return false;
    bool ok = len <= APP_META_MAX && app_meta_parse((const char *)data, len, m);
    free(data);
    return ok;
}

const char *app_meta_api_problem(const app_meta_t *m, char *buf, size_t cap)
{
    int api = m->lua_api ? m->lua_api : 1;
    if (api < 0) snprintf(buf, cap, "app.json: lua_api isn't a whole number");
    else if (api > APP_LUA_API) snprintf(buf, cap, "needs newer firmware: lua_api %d, this has %d", api, APP_LUA_API);
    else if (api < APP_LUA_API_MIN)
        snprintf(buf, cap, "made for older firmware: lua_api %d, runs %d-%d", api, APP_LUA_API_MIN, APP_LUA_API);
    else return NULL;
    return buf;
}

bool app_meta_runs_on(const app_meta_t *m, int api_min, int api)
{
    int v = m->lua_api ? m->lua_api : 1;
    return v >= api_min && v <= api;
}
