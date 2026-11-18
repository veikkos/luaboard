/* The marketplace index. See apps_index.h. */
#include "apps_index.h"

#include <stdio.h>
#include <string.h>

#include "hex.h"
#include "json.h"
#include "names.h"
#include "semver.h"


/* One version of an app, as far as choosing between them goes. */
typedef struct {
    char version[24];
    int lua_api;
    char commit[41];
    uint32_t files_at;
} version_t;

static bool fail(char *err, size_t cap, const char *why)
{
    if (cap) snprintf(err, cap, "%s", why);
    return false;
}

/* A version object at v. True if it is one this firmware can install. */
static bool read_version(const char *text, size_t len, json_t *v, int api_min, int api, version_t *out)
{
    memset(out, 0, sizeof *out);
    out->lua_api = 1;
    bool have_files = false, cut = false;
    char key[16];
    while (json_member(v, key, sizeof key)) {
        long n;
        if (!strcmp(key, "version")) json_string(v, out->version, sizeof out->version, &cut);
        else if (!strcmp(key, "lua_api")) { if (json_whole(v, &n)) out->lua_api = (int)n; }
        else if (!strcmp(key, "commit")) json_string(v, out->commit, sizeof out->commit, NULL);
        else if (!strcmp(key, "files")) {
            json_peek(v);
            out->files_at = (uint32_t)(v->p - text);
            have_files = json_value(v, NULL, 0);
        } else json_value(v, NULL, 0);
    }
    if (v->err) return false;
    semver_t sv;
    uint8_t commit[20];
    if (cut || !semver_parse(out->version, &sv) || out->lua_api < api_min || out->lua_api > api ||
        strlen(out->commit) != 40 || hex_decode(out->commit, commit, sizeof commit) != 20 || !have_files)
        return false;
    /* Files a board can install: some, each loadable, main.lua among them. */
    static apps_file_t files[APPS_INDEX_MAX_FILES];
    int n = apps_index_files(text, len, &(apps_entry_t){ .files_at = out->files_at }, files, APPS_INDEX_MAX_FILES);
    for (int i = 0; i < n; i++)
        if (!strcmp(files[i].path, "main.lua")) return true;
    return false;
}

/* The app object at a, named name: into *e if some version of it runs here.
   False only if the text is broken (a->err). */
static bool read_app(const char *text, size_t len, json_t *a, int api_min, int api, apps_entry_t *e, bool *runs)
{
    version_t best = { 0 }, ver;
    bool have = false;
    char key[16];
    *runs = false;
    while (json_member(a, key, sizeof key)) {
        if (!strcmp(key, "title")) json_string(a, e->title, sizeof e->title, NULL);
        else if (!strcmp(key, "description")) json_string(a, e->description, sizeof e->description, NULL);
        else if (!strcmp(key, "author")) json_string(a, e->author, sizeof e->author, NULL);
        else if (!strcmp(key, "license")) json_string(a, e->license, sizeof e->license, NULL);
        else if (!strcmp(key, "needs_setup")) {
            char b[8];
            e->needs_setup = json_value(a, b, sizeof b) && !strcmp(b, "true");
        }
        else if (!strcmp(key, "versions")) {
            bool first = true;
            while (json_array_next(a, &first)) {
                json_t v;
                if (!json_object(a, &v)) break;
                if (read_version(text, len, &v, api_min, api, &ver) && (!have || semver_cmp(ver.version, best.version) > 0)) {
                    best = ver;
                    have = true;
                }
                if (v.err) return json_fail(a, v.err);
            }
        } else json_value(a, NULL, 0);
    }
    if (a->err) return false;
    if (have) {
        memcpy(e->version, best.version, sizeof e->version);
        e->lua_api = best.lua_api;
        memcpy(e->commit, best.commit, sizeof e->commit);
        e->files_at = best.files_at;
        *runs = true;
    }
    return true;
}

bool apps_index_parse(const char *text, size_t len, int api_min, int api, apps_index_t *out, char *err, size_t cap)
{
    memset(out, 0, sizeof *out);
    json_t j, apps;
    char key[16];
    long format = -1;
    bool have_apps = false;
    json_init(&j, text, len);
    while (json_member(&j, key, sizeof key)) {
        if (!strcmp(key, "format")) json_whole(&j, &format);
        else if (!strcmp(key, "apps")) have_apps = json_object(&j, &apps);
        else json_value(&j, NULL, 0);
    }
    if (j.err) return fail(err, cap, j.err);
    if (format < 0) return fail(err, cap, "no format");
    if (format > APPS_INDEX_FORMAT) {
        out->newer_format = true;
        return true;
    }
    if (!have_apps) return fail(err, cap, "no apps");

    char name[APPS_NAME_MAX];
    while (json_member(&apps, name, sizeof name)) {
        json_t a;
        if (!json_object(&apps, &a)) break;
        if (out->n == APPS_INDEX_MAX_APPS || !name_valid(name, strlen(name), APPS_NAME_MAX)) continue;
        apps_entry_t *e = &out->app[out->n];
        memset(e, 0, sizeof *e);
        memcpy(e->name, name, sizeof e->name);
        bool runs;
        if (!read_app(text, len, &a, api_min, api, e, &runs)) return fail(err, cap, a.err);
        if (runs) out->n++;
    }
    if (apps.err) return fail(err, cap, apps.err);
    return true;
}

int apps_index_files(const char *text, size_t len, const apps_entry_t *e, apps_file_t *out, int cap)
{
    if (e->files_at >= len) return -1;
    json_t j;
    json_init(&j, text + e->files_at, len - e->files_at);
    bool first = true;
    int n = 0;
    while (json_array_next(&j, &first)) {
        json_t f;
        apps_file_t file = { 0 };
        char key[8], sha[65] = "";
        bool cut = false, have_size = false;
        if (!json_object(&j, &f)) return -1;
        while (json_member(&f, key, sizeof key)) {
            long size;
            if (!strcmp(key, "path")) json_string(&f, file.path, sizeof file.path, &cut);
            else if (!strcmp(key, "size")) { if ((have_size = json_whole(&f, &size))) file.size = (uint32_t)size; }
            else if (!strcmp(key, "sha256")) json_string(&f, sha, sizeof sha, NULL);
            else json_value(&f, NULL, 0);
        }
        if (f.err || cut || !have_size || file.size > APPS_FILE_MAX ||
            !name_valid(file.path, strlen(file.path), APPS_PATH_MAX) || strlen(sha) != 64 ||
            hex_decode(sha, file.sha256, sizeof file.sha256) != 32 || n == cap)
            return -1;
        if (out) out[n] = file;
        n++;
    }
    return j.err ? -1 : n;
}

const apps_entry_t *apps_index_find(const apps_index_t *ix, const char *name)
{
    for (int i = 0; i < ix->n; i++)
        if (!strcmp(ix->app[i].name, name)) return &ix->app[i];
    return NULL;
}
