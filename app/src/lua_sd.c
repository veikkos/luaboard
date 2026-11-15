/*
 * Lua `sd` module: files on the microSD card, whole or in parts, with no
 * file handles to leave open. The card has to be FAT formatted.
 *
 *   sd.info() -> total, free                     bytes
 *   sd.list([dir]) -> { { name, size, dir }, ... }   sorted by name
 *   sd.exists(path) -> "file" | "dir" | false
 *   sd.size(path) -> bytes
 *   sd.read(path, [offset], [n]) -> data          n up to 65536 (the default); "" past the end
 *   sd.write(path, data) -> true                 makes or replaces the file
 *   sd.append(path, data) -> true
 *   sd.mkdir(path) -> true
 *   sd.remove(path) -> true                      a file, or an empty folder
 *
 * Each returns nil and why when it can't (no card, no such file, ...). A
 * bad path is an error. The card is mounted by the first call and let go
 * of when the script ends.
 *
 * The card shares a pin with the display (sdcard.h): every card access is
 * between sdcard_begin() and sdcard_end(), and nothing in between may
 * raise a Lua error, so results are built for Lua only after sdcard_end().
 */
#include "lua_sd.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "hw.h"
#include "lauxlib.h"
#include "script.h"
#include "sdcard.h"

#define PATH_MAX_LEN  200            /* of a path on the card, in bytes */
#define FULL_MAX      (PATH_MAX_LEN + 528)   /* ... with the card's root before it (the simulator's is a host folder) */
#define READ_MAX      65536
#define LIST_MAX      512            /* entries of a folder */
#define NAME_MAX_LEN  128            /* longer names are left out of a listing */
#define SLOW_MS       9000           /* a card operation's time that isn't the script's (the watchdog) */

void lua_sd_release(void)
{
    sdcard_begin();
    sdcard_unmount();
    sdcard_end();
}

/* The path argument, as a path under the card's root ("" for the root
   itself, if `optional` and not given). A path that could leave the card,
   or that FAT can't hold or would change (it drops a name's trailing dots
   and spaces), is a Lua error. */
static const char *path_arg(lua_State *L, int idx, bool optional)
{
    if (optional && lua_isnoneornil(L, idx)) return "";
    luaL_argcheck(L, lua_type(L, idx) == LUA_TSTRING, idx, "a path (string)");
    size_t len;
    const char *p = lua_tolstring(L, idx, &len);
    while (*p == '/') p++, len--;                    /* "/log.txt" is "log.txt" */
    luaL_argcheck(L, len >= 1 && len <= PATH_MAX_LEN, idx, "a path of 1..200 characters");
    luaL_argcheck(L, strlen(p) == len, idx, "a path without NUL characters");
    for (const char *c = p; *c; c++)
        luaL_argcheck(L, (unsigned char)*c >= 0x20 && *c != 0x7F && !strchr("\\:*?\"<>|", *c), idx,
                      "a path without \\ : * ? \" < > | or control characters");
    for (const char *part = p; *part;) {             /* no empty, "." or ".." parts */
        size_t n = strcspn(part, "/");
        bool dots = (n == 1 && part[0] == '.') || (n == 2 && part[0] == '.' && part[1] == '.');
        luaL_argcheck(L, n > 0 && !dots, idx, "a path without empty, . or .. parts");
        luaL_argcheck(L, part[n - 1] != '.' && part[n - 1] != ' ', idx, "names that don't end in a dot or a space");
        part += n;
        if (*part == '/') {
            part++;
            luaL_argcheck(L, *part, idx, "a path that doesn't end in /");
        }
    }
    return p;
}

static void full_path(char *out, size_t cap, const char *path)
{
    if (*path) snprintf(out, cap, "%s/%s", sdcard_root(), path);
    else snprintf(out, cap, "%s", sdcard_root());
}

static const char *why(int err)
{
    switch (err) {
    case ENOENT: case ENOTDIR: return "no such file or folder";
    case EEXIST: return "it's there already";
    case ENOTEMPTY: return "the folder isn't empty";
    case ENOSPC: return "the card is full";
    case EISDIR: return "it's a folder";
    case EACCES: case EROFS: case EPERM: return "the card can't be written";
    default: return "card error";
    }
}

/* Every call's frame: the card's pin handed over, the card mounted. `err`
   is NULL when the card is there. */
typedef struct { uint32_t t0; const char *err; } op_t;

static op_t op_begin(void)
{
    op_t op = { .t0 = hw_millis() };
    sdcard_begin();
    op.err = sdcard_mount();
    return op;
}

/* A file operation failed (errno): why, and is the card still there? */
static void op_failed(op_t *op)
{
    op->err = why(errno);
    sdcard_check();
}

/* Ends the frame. Returns the Lua results for a failure (nil, why), or 0. */
static int op_end(lua_State *L, op_t *op)
{
    sdcard_end();
    script_watchdog_credit(L, op->t0, SLOW_MS);
    if (!op->err) return 0;
    lua_pushnil(L);
    lua_pushstring(L, op->err);
    return 2;
}

static int l_info(lua_State *L)
{
    uint64_t total = 0, free_bytes = 0;
    op_t op = op_begin();
    if (!op.err && !sdcard_info(&total, &free_bytes)) { errno = EIO; op_failed(&op); }
    int n = op_end(L, &op);
    if (n) return n;
    lua_pushinteger(L, (lua_Integer)total);
    lua_pushinteger(L, (lua_Integer)free_bytes);
    return 2;
}

/* "file", "dir" or NULL (errno) for a path on the mounted card. */
static const char *kind_of(const char *path, long *size)
{
    char full[FULL_MAX];
    struct stat st;
    full_path(full, sizeof full, path);
    if (stat(full, &st) != 0) return NULL;
    if (size) *size = S_ISDIR(st.st_mode) ? 0 : (long)st.st_size;
    return S_ISDIR(st.st_mode) ? "dir" : "file";
}

static int l_exists(lua_State *L)
{
    const char *path = path_arg(L, 1, false), *kind = NULL;
    op_t op = op_begin();
    if (!op.err) {
        kind = kind_of(path, NULL);
        if (!kind && errno != ENOENT && errno != ENOTDIR) op_failed(&op);
    }
    int n = op_end(L, &op);
    if (n) return n;
    if (kind) lua_pushstring(L, kind);
    else lua_pushboolean(L, 0);
    return 1;
}

static int l_size(lua_State *L)
{
    const char *path = path_arg(L, 1, false);
    long size = 0;
    op_t op = op_begin();
    if (!op.err && !kind_of(path, &size)) op_failed(&op);
    int n = op_end(L, &op);
    if (n) return n;
    lua_pushinteger(L, size);
    return 1;
}

typedef struct { char name[NAME_MAX_LEN]; long size; bool dir; } entry_t;

static int by_name(const void *a, const void *b) { return strcmp(((const entry_t *)a)->name, ((const entry_t *)b)->name); }

static int l_list(lua_State *L)
{
    const char *path = path_arg(L, 1, true);
    entry_t *list = lua_newuserdatauv(L, LIST_MAX * sizeof *list, 0);   /* Lua's, so an error can't leak it */
    int count = 0;
    op_t op = op_begin();
    if (!op.err) {
        char full[FULL_MAX];
        full_path(full, sizeof full, path);
        DIR *d = opendir(full);
        if (!d) op_failed(&op);
        else {
            /* A read that fails midway (the card pulled, a bad sector) is
               a failure, not a shorter list: readdir's NULL is the end only
               with errno untouched. */
            struct dirent *e;
            while (count < LIST_MAX) {
                errno = 0;
                if (!(e = readdir(d))) {
                    if (errno) op_failed(&op);
                    break;
                }
                size_t len = strlen(e->d_name);
                if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..") || len >= NAME_MAX_LEN) continue;
                char p[FULL_MAX + 1 + NAME_MAX_LEN];
                struct stat st;
                snprintf(p, sizeof p, "%s/%.*s", full, NAME_MAX_LEN - 1, e->d_name);
                if (stat(p, &st) != 0) {
                    if (errno == ENOENT) continue;           /* gone since it was listed */
                    op_failed(&op);
                    break;
                }
                memcpy(list[count].name, e->d_name, len + 1);
                list[count].dir = S_ISDIR(st.st_mode);
                list[count].size = list[count].dir ? 0 : (long)st.st_size;
                count++;
            }
            closedir(d);
        }
    }
    int n = op_end(L, &op);
    if (n) return n;
    qsort(list, (size_t)count, sizeof *list, by_name);
    lua_createtable(L, count, 0);
    for (int i = 0; i < count; i++) {
        lua_createtable(L, 0, 3);
        lua_pushstring(L, list[i].name); lua_setfield(L, -2, "name");
        lua_pushinteger(L, list[i].size); lua_setfield(L, -2, "size");
        lua_pushboolean(L, list[i].dir); lua_setfield(L, -2, "dir");
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

static int l_read(lua_State *L)
{
    const char *path = path_arg(L, 1, false);
    lua_Integer offset = luaL_optinteger(L, 2, 0), want = luaL_optinteger(L, 3, READ_MAX);
    luaL_argcheck(L, offset >= 0 && offset <= 0x7FFFFFFF, 2, "an offset of 0 or more");
    luaL_argcheck(L, want >= 1 && want <= READ_MAX, 3, "1..65536 bytes");
    char *buf = lua_newuserdatauv(L, (size_t)want, 0);       /* Lua's, so an error can't leak it */
    size_t got = 0;
    op_t op = op_begin();
    if (!op.err) {
        char full[FULL_MAX];
        full_path(full, sizeof full, path);
        errno = 0;
        FILE *f = fopen(full, "rb");
        if (!f) op_failed(&op);
        else {
            /* A seek past the end is fine (it reads ""); a seek that fails
               is the card's error. */
            int seek_errno = fseek(f, (long)offset, SEEK_SET) == 0 ? 0 : (errno ? errno : EIO);
            if (!seek_errno) got = fread(buf, 1, (size_t)want, f);
            bool bad = !seek_errno && ferror(f);
            fclose(f);                                   /* before the card may be let go of */
            if (seek_errno) { errno = seek_errno; op_failed(&op); }
            else if (bad) { errno = EIO; op_failed(&op); }
        }
    }
    int n = op_end(L, &op);
    if (n) return n;
    lua_pushlstring(L, buf, got);
    return 1;
}

static int write_file(lua_State *L, const char *mode)
{
    const char *path = path_arg(L, 1, false);
    size_t len;
    const char *data = luaL_checklstring(L, 2, &len);
    op_t op = op_begin();
    if (!op.err) {
        char full[FULL_MAX];
        full_path(full, sizeof full, path);
        errno = 0;
        FILE *f = fopen(full, mode);
        if (!f) op_failed(&op);
        else {
            bool ok = fwrite(data, 1, len, f) == len;
            if (fclose(f) != 0) ok = false;              /* closing writes it to the card */
            if (!ok) { if (!errno) errno = EIO; op_failed(&op); }
        }
    }
    int n = op_end(L, &op);
    if (n) return n;
    lua_pushboolean(L, 1);
    return 1;
}

static int l_write(lua_State *L) { return write_file(L, "wb"); }
static int l_append(lua_State *L) { return write_file(L, "ab"); }

static int l_mkdir(lua_State *L)
{
    const char *path = path_arg(L, 1, false);
    op_t op = op_begin();
    if (!op.err) {
        char full[FULL_MAX];
        full_path(full, sizeof full, path);
        if (mkdir(full, 0755) != 0) op_failed(&op);
    }
    int n = op_end(L, &op);
    if (n) return n;
    lua_pushboolean(L, 1);
    return 1;
}

static int l_remove(lua_State *L)
{
    const char *path = path_arg(L, 1, false);
    op_t op = op_begin();
    if (!op.err) {
        char full[FULL_MAX];
        full_path(full, sizeof full, path);
        const char *kind = kind_of(path, NULL);
        if (!kind || (kind[0] == 'd' ? rmdir(full) : unlink(full)) != 0) {
            /* FAT refuses a folder with something in it as "denied". */
            if (kind && kind[0] == 'd' && errno == EACCES) errno = ENOTEMPTY;
            op_failed(&op);
        }
    }
    int n = op_end(L, &op);
    if (n) return n;
    lua_pushboolean(L, 1);
    return 1;
}

static const luaL_Reg sd_funcs[] = {
    { "info", l_info }, { "list", l_list }, { "exists", l_exists }, { "size", l_size }, { "read", l_read },
    { "write", l_write }, { "append", l_append }, { "mkdir", l_mkdir }, { "remove", l_remove }, { NULL, NULL },
};

void lua_sd_register(lua_State *L)
{
    luaL_newlib(L, sd_funcs);
    lua_setglobal(L, "sd");
}
