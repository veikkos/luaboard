#include "protocol.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "app_config.h"
#include "app_update.h"
#include "menu.h"
#include "appdata.h"
#include "crash.h"
#include "ble.h"
#include "auth.h"
#include "board.h"
#include "clock.h"
#include "crc32.h"
#include "esp_log.h"
#include "esp_random.h"
#include "hw.h"
#include "lua.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "gfx.h"
#include "hex.h"
#include "net.h"
#include "ota.h"
#include "browser.h"
#include "buttons.h"
#include "screens.h"
#include "script.h"
#include "secrets.h"
#include "settings.h"
#include "tun.h"
#include "update.h"
#include "vfs.h"

static const char *TAG = "proto";

/* ST_DRAIN: an upload timed out; the rest of it may still come and must
   not run as commands, so input is dropped until the link goes quiet. */
typedef enum { ST_LINE, ST_UPLOAD, ST_OTA, ST_DRAIN } state_t;

/* SECRET PART pieces so far: one value at a time. */
typedef struct {
    char app[VFS_APP_NAME_MAX], name[16];
    char *buf;
    size_t len;
} secret_part_t;

/* One protocol session per link (USB serial, Wi-Fi TCP client, ...). Each has
   its own line buffer and upload state; replies go to the session that sent
   the command, LOG/EVT lines go to every session. */
typedef struct {
    proto_link_t link;
    bool used;
    state_t state;
    char line[PROTO_LINE_MAX];
    size_t line_len;
    bool line_overflow;
    bool authed;                         /* Wi-Fi: its link proved it has the pairing key */
    struct {
        char name[VFS_MAX_NAME];
        uint8_t *buf;
        size_t size, got;
        uint32_t crc;
        uint32_t last_ms;
    } up;
    secret_part_t part;
} session_t;

static session_t s_sessions[PROTO_MAX_LINKS];
static session_t *s_cur;   /* session whose command is being handled */

/* May this session use the device? USB always; Wi-Fi only once its link
   has proved it holds the pairing key (wifilink.c), and never while the
   device isn't paired. Evaluated live, so unpairing locks a client at once. */
static bool session_open(const session_t *s)
{
    return s->link.trusted || (auth_paired() && s->authed);
}

/* ------------------------------------------------------------- output */

static bool sess_write(session_t *s, const void *data, size_t len)
{
    return s && s->used && s->link.write(s->link.ctx, data, len);
}

static void sess_puts(session_t *s, const char *str) { sess_write(s, str, strlen(str)); }

/* Send "<prefix> <text>\n" to one session, flattening newlines so one message = one line. */
static void send_line_to(session_t *s, const char *prefix, const char *text)
{
    sess_puts(s, prefix);
    if (text && *text) {
        sess_write(s, " ", 1);
        for (const char *p = text; *p; ) {
            size_t n = strcspn(p, "\r\n");
            sess_write(s, p, n);
            p += n;
            if (*p) { sess_write(s, " ", 1); p++; }
        }
    }
    sess_write(s, "\n", 1);
}

/* Reply/data lines: to the session that sent the command. */
static void send_line(const char *prefix, const char *text) { send_line_to(s_cur, prefix, text); }

/* Raw payload bytes (GET, FB) to the current session. */
static void send_raw(const void *data, size_t len) { sess_write(s_cur, data, len); }

/* A command's final line: "OK ..." or "ERR ...". */
static void reply(const char *kind, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void reply(const char *kind, const char *fmt, ...)
{
    char buf[PROTO_MSG_MAX];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    send_line(kind, buf);
}
#define reply_ok(...)  reply("OK", __VA_ARGS__)
#define reply_err(...) reply("ERR", __VA_ARGS__)

void proto_log(const char *text)
{
    /* Each source line becomes its own LOG line, on every link. */
    for (int i = 0; i < PROTO_MAX_LINKS; i++) {
        session_t *s = &s_sessions[i];
        if (!s->used || !session_open(s)) continue;   /* no logs before login */
        const char *p = text;
        do {
            size_t n = strcspn(p, "\n");
            sess_puts(s, "LOG ");
            sess_write(s, p, n);
            sess_write(s, "\n", 1);
            p += n;
            if (*p == '\n') p++;
        } while (*p);
    }
}

void proto_eventf(const char *fmt, ...)
{
    char buf[PROTO_MSG_MAX];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    for (int i = 0; i < PROTO_MAX_LINKS; i++)
        if (s_sessions[i].used && session_open(&s_sessions[i])) send_line_to(&s_sessions[i], "EVT", buf);
}

/* ------------------------------------------------------------ helpers */

/* Split off the first whitespace-separated word; returns the rest (trimmed). */
static char *next_word(char **s)
{
    char *p = *s;
    while (*p == ' ') p++;
    char *w = p;
    while (*p && *p != ' ') p++;
    if (*p) *p++ = 0;
    while (*p == ' ') p++;
    *s = p;
    return w;
}

/* ----------------------------------------------------------- commands */

static void cmd_info(char *args)
{
    size_t used, peak, limit, ram, fused, ftotal;
    script_heap_stats(&used, &peak, &limit);
    vfs_usage(&ram, &fused, &ftotal);
    int napps = vfs_app_count();
    const char *autostart = settings_autostart();
    char now[40];
    clock_iso_local(clock_now(), now, sizeof now);
    const update_manifest_t *upd = update_available();
    reply_ok("fw=%s ver=%s board=%s model=%s lua=%s api=%d apps=%d flash=%zu/%zu ram=%zu/%d heap=%zu/%zu running=%s autorun=%s ip=%s auth=%s time=%s target=%s slot=%s ota=%s screen=%s reset=%s update=%s app_updates=%d",
             FW_NAME, fw_version(), BOARD_NAME, hw_model(), LUA_VERSION_MAJOR "." LUA_VERSION_MINOR "." LUA_VERSION_RELEASE,
             APP_LUA_API, napps, fused, ftotal, ram, VFS_RAM_CAPACITY, used, limit,
             script_running() ? script_current() : "-", autostart[0] ? autostart : "-", net_ip(),
             auth_paired() ? "key" : "none", now, CONFIG_IDF_TARGET, ota_slot(), ota_state(),
             hw_screen_is_off() ? "off" : "on", hw_reset_reason(),
             upd ? upd->version : update_manual() ? "manual" : "-", app_update_count());
}

static void ls_line(const char *path, size_t size, uint32_t crc, void *ctx)
{
    (void)ctx;
    char buf[128];
    snprintf(buf, sizeof buf, "%s %zu %08x", path, size, (unsigned)crc);
    send_line("FILE", buf);
}

static void cmd_ls(char *args) { reply_ok("%d", vfs_list(ls_line, NULL)); }

/* CRASH: the last crash's record, if the last reset was one: FRAME lines
   (the innermost first), then OK ver=.. cause=..; OK none without one. */
static void cmd_crash(char *args)
{
    const crash_record_t *c = crash_last();
    if (!c) { reply_ok("none"); return; }
    for (uint32_t i = 0; i < c->frames; i++) send_line("FRAME", c->frame[i]);
    reply_ok("ver=%s frames=%u cause=%s", c->version, (unsigned)c->frames, c->cause);
}

static const char *state_name(vfs_app_state_t st)
{
    return st == VFS_INSTALLED ? "installed" : st == VFS_RAM ? "ram" : "modified";
}

/* APPS -> APP <name> <files> <bytes> installed|ram|modified lines, OK <count> */
static void cmd_apps(char *args)
{
    vfs_app_t apps[VFS_MAX_APPS];
    int n = vfs_apps(apps, VFS_MAX_APPS);
    for (int i = 0; i < n; i++) {
        char buf[96];
        snprintf(buf, sizeof buf, "%s %d %zu %s", apps[i].name, apps[i].files, apps[i].bytes,
                 state_name(apps[i].state));
        send_line("APP", buf);
    }
    reply_ok("%d", n);
}

static const char *store_err(esp_err_t err)
{
    return err == ESP_ERR_NO_MEM ? "flash full" : err == ESP_ERR_NOT_FOUND ? "no such app"
         : err == ESP_ERR_INVALID_STATE ? "apps partition not mounted" : esp_err_to_name(err);
}

static bool install_has(const char *app);

static void cmd_put(char *args)
{
    char *name = next_word(&args);
    char *size_s = next_word(&args);
    char *crc_s = next_word(&args);
    char *end;
    long size = strtol(size_s, &end, 10);
    char *crc_end;
    unsigned long crc = strtoul(crc_s, &crc_end, 16);
    if (!*name || !*size_s || *end || !*crc_s || *crc_end || strlen(crc_s) > 8 || !isxdigit((unsigned char)*crc_s)) {
        reply_err("usage: PUT <app>/<file> <size> <crc32hex>");
        return;
    }
    if (!vfs_valid_path(name)) {
        reply_err("bad name '%s': use <app>/<file>, names of A-Z a-z 0-9 _ . - (app up to %d, file up to %d characters)",
                  name, VFS_APP_NAME_MAX - 1, VFS_FILE_NAME_MAX - 1);
        return;
    }
    if (size < 0 || size > VFS_FILE_MAX) { reply_err("size out of range (max %d)", VFS_FILE_MAX); return; }
    char app[VFS_APP_NAME_MAX], other[VFS_APP_NAME_MAX];
    vfs_app_of(name, app, sizeof app);
    if (vfs_app_case_clash(app, other, sizeof other)) {
        reply_err("there is an app '%s' already: an app name can't differ from another only in letter case", other);
        return;
    }

    free(s_cur->up.buf);
    s_cur->up.buf = malloc(size ? (size_t)size : 1);
    if (!s_cur->up.buf) { reply_err("out of memory"); return; }
    snprintf(s_cur->up.name, sizeof s_cur->up.name, "%s", name);
    s_cur->up.size = (size_t)size;
    s_cur->up.got = 0;
    s_cur->up.crc = (uint32_t)crc;
    s_cur->up.last_ms = hw_millis();
    s_cur->state = ST_UPLOAD;
    reply_ok("READY");
}

static void upload_finish(void)
{
    s_cur->state = ST_LINE;
    uint32_t crc = crc32_calc(s_cur->up.buf, s_cur->up.size);
    if (crc != s_cur->up.crc) {
        reply_err("crc mismatch: got %08x expected %08x", (unsigned)crc, (unsigned)s_cur->up.crc);
    } else {
        esp_err_t err = vfs_write(s_cur->up.name, s_cur->up.buf, s_cur->up.size);
        if (err == ESP_OK) {
            char app[VFS_APP_NAME_MAX];
            vfs_app_of(s_cur->up.name, app, sizeof app);
            script_set_default_app(app);
            reply_ok("STORED %s %zu", s_cur->up.name, s_cur->up.size);
        } else {
            reply_err("store failed: %s", err == ESP_ERR_NO_MEM ? "RAM for uploads full (install or remove some)"
                                                                : esp_err_to_name(err));
        }
    }
    free(s_cur->up.buf);
    s_cur->up.buf = NULL;
}

/* OTA <size> <sha256hex>: a firmware image follows, streamed into the free
   app slot (ota.c), never held in RAM whole. */
static void cmd_ota(char *args)
{
    char *size_s = next_word(&args), *sha_s = next_word(&args), *end;
    long size = strtol(size_s, &end, 10);
    uint8_t sha[32];
    bool ok = *size_s && !*end && hex_decode(sha_s, sha, sizeof sha) == (long)sizeof sha;
    if (!ok || size <= 0) { reply_err("usage: OTA <size> <sha256hex>"); return; }
    char err[200];
    if (!ota_begin((size_t)size, sha, err, sizeof err)) { reply_err("%s", err); return; }
    s_cur->up.last_ms = hw_millis();
    s_cur->state = ST_OTA;
    reply_ok("READY");
}

static void ota_upload_finish(void)
{
    s_cur->state = ST_LINE;
    char msg[200];
    if (ota_finish(msg, sizeof msg)) reply_ok("UPDATED %s", msg);
    else reply_err("%s", msg);
}

static void cmd_get(char *args)
{
    char *name = next_word(&args), path[VFS_MAX_NAME];
    size_t len;
    uint8_t *data = script_resolve(name, path, sizeof path) ? vfs_load(path, &len) : NULL;
    if (!data) { reply_err("no such file: %s", name); return; }
    reply_ok("%zu %08x", len, (unsigned)crc32_calc(data, len));
    send_raw(data, len);
    free(data);
}

/* Framebuffer snapshot for live preview on the PC:
   "OK <w> <h> rgb565 <bytes> <crc32>" then w*h little-endian RGB565 pixels.
   FB <crc32>: just "OK SAME" if the picture still has that CRC. */
static void cmd_fb(char *args)
{
    const uint16_t *fb = gfx_framebuffer();
    int w = gfx_width(), h = gfx_height();
    size_t n = (size_t)w * h * 2;
    if (!fb) { reply_err("no framebuffer"); return; }
    char *have = next_word(&args), *end;
    unsigned crc = (unsigned)crc32_calc(fb, n);
    if (*have) {
        unsigned long prev = strtoul(have, &end, 16);
        if (!*end && prev == crc) { reply_ok("SAME"); return; }
    }
    reply_ok("%d %d rgb565 %zu %08x", w, h, n, crc);
    send_raw(fb, n);
}

/* TOUCH <x> <y> | TOUCH UP: a finger from the PC, in the touch area (the
   button strip below the screen included). Repeat TOUCH <x> <y> while it's
   held: it's let go after REMOTE_TOUCH_MS without one, or at once when the
   link that put it down goes away. Only an open session gets here (USB, or
   a logged-in Wi-Fi client): the screen offers nothing such a client can't
   do with commands anyway, and pairing isn't on it. */
static int s_touch_link = -1;   /* the session whose finger is down, -1: none */

static void cmd_touch(char *args)
{
    char *a = next_word(&args), *b = next_word(&args), *extra = next_word(&args), *e1, *e2;
    if (!strcasecmp(a, "UP") && !*b) {
        buttons_remote(false, 0, 0);
        s_touch_link = -1;
        reply_ok("TOUCH UP");
        return;
    }
    long x = strtol(a, &e1, 10), y = strtol(b, &e2, 10);
    if (!*a || !*b || *extra || *e1 || *e2 || x < 0 || x >= BOARD_TOUCH_W || y < 0 || y >= BOARD_TOUCH_H) {
        reply_err("usage: TOUCH <x 0..%d> <y 0..%d> | TOUCH UP", BOARD_TOUCH_W - 1, BOARD_TOUCH_H - 1);
        return;
    }
    buttons_remote(true, (int)x, (int)y);
    s_touch_link = (int)(s_cur - s_sessions);
    reply_ok("TOUCH %ld %ld", x, y);
}

/* KEY POWER: a short press of the power key (home). */
static void cmd_key(char *args)
{
    char *k = next_word(&args);
    if (strcasecmp(k, "POWER") || *next_word(&args)) { reply_err("usage: KEY POWER"); return; }
    buttons_remote_power();
    reply_ok("KEY POWER");
}

/* The app on the screen (running, or stopped on its error screen) if it's
   `app`, or any app when `app` is NULL. Fills `path` with its "<app>/<file>". */
static bool app_on_screen(const char *app, char *path, size_t cap)
{
    char cur[VFS_APP_NAME_MAX];
    script_app(cur, sizeof cur);
    if (!*cur || (app && strcmp(cur, app))) return false;
    if (!script_running() && !screen_error_active()) return false;
    snprintf(path, cap, "%s", script_last());
    return true;
}

/* After INSTALL/SAVE: start the app again so the new code shows. */
static void restart(const char *path)
{
    char err[200];
    if (script_run(path, err, sizeof err)) proto_eventf("started %s", script_current());
    else proto_eventf("error %s", err);            /* the error screen is up */
}

static void cmd_run(char *args)
{
    char *name = next_word(&args), app[VFS_APP_NAME_MAX];
    if (!*name) name = "main.lua";
    vfs_app_of(name, app, sizeof app);
    if (install_has(app)) return;
    char err[200];
    if (script_run(name, err, sizeof err)) {
        reply_ok("RUNNING %s", script_current());
        proto_eventf("started %s", script_current());
    } else {
        reply_err("%s", err);
    }
}

/* Decode hex into a NUL-terminated string. False on bad input, or if the
   bytes contain a NUL (it would silently cut the string short). */
static bool unhex(const char *hex, char *out, size_t cap)
{
    if (cap == 0) return false;
    long n = hex_decode(hex, out, cap - 1);
    if (n < 0 || memchr(out, 0, (size_t)n)) return false;
    out[n] = 0;
    return true;
}

/* WIFI                       status
   WIFI SET <ssid-hex> [<password-hex>]   store + connect (hex: spaces etc. are safe)
   WIFI OFF                   forget the network, Wi-Fi off */
static void cmd_wifi(char *args)
{
    char *sub = next_word(&args);
    for (char *c = sub; *c; c++) if (*c >= 'a' && *c <= 'z') *c -= 32;
    char status[192];
    if (!strcmp(sub, "SET")) {
        char *ssid_hex = next_word(&args), *pass_hex = next_word(&args);
        char ssid[33], pass[65];
        if (!*ssid_hex || !unhex(ssid_hex, ssid, sizeof ssid) || !unhex(pass_hex, pass, sizeof pass)) {
            reply_err("usage: WIFI SET <ssid-hex> [<password-hex>] (ssid <= 32, password <= 64 bytes)");
            return;
        }
        esp_err_t err = net_set_credentials(ssid, pass);
        if (err != ESP_OK) { reply_err("wifi: %s", esp_err_to_name(err)); return; }
    } else if (!strcmp(sub, "OFF")) {
        esp_err_t err = net_set_credentials("", "");
        if (err != ESP_OK) { reply_err("wifi: %s", esp_err_to_name(err)); return; }
    } else if (*sub) {
        reply_err("usage: WIFI | WIFI SET <ssid-hex> [<password-hex>] | WIFI OFF");
        return;
    }
    net_status(status, sizeof status);
    reply_ok("%s", status);
}

/* PAIR | PAIR NEW | PAIR KEY | PAIR OFF   (all but the first: USB only)
   PAIR      -> OK paired=on id=<16 hex> | OK paired=off
   PAIR NEW  -> OK PAIR <id> <key: 64 hex>   a new key; Wi-Fi clients are dropped
   PAIR KEY  -> OK PAIR <id> <key: 64 hex>   the current key, for another computer
   PAIR OFF  -> OK paired=off                Wi-Fi takes no commands; clients dropped */
static void cmd_pair(char *args)
{
    char *sub = next_word(&args);
    for (char *c = sub; *c; c++) if (*c >= 'a' && *c <= 'z') *c -= 32;
    if (*sub && !s_cur->link.trusted) { reply_err("pairing only works over USB"); return; }
    char id[AUTH_ID_HEX];
    if (!strcmp(sub, "NEW") || !strcmp(sub, "KEY")) {
        if (!strcmp(sub, "NEW")) {
            esp_err_t err = auth_pair_new();
            if (err != ESP_OK) { reply_err("pair: %s", esp_err_to_name(err)); return; }
            net_drop_clients("paired anew");
        }
        uint8_t key[AUTH_KEY_LEN];
        if (!auth_key(key)) { reply_err("not paired (PAIR NEW makes a key)"); return; }
        char hex[2 * AUTH_KEY_LEN + 1];
        hex_encode(key, sizeof key, hex);
        memset(key, 0, sizeof key);
        auth_key_id(id);
        reply_ok("PAIR %s %s", id, hex);
        memset(hex, 0, sizeof hex);
        return;
    }
    if (!strcmp(sub, "OFF")) {
        esp_err_t err = auth_unpair();
        if (err != ESP_OK) { reply_err("pair: %s", esp_err_to_name(err)); return; }
        net_drop_clients("unpaired");
    } else if (*sub) {
        reply_err("usage: PAIR | PAIR NEW | PAIR KEY | PAIR OFF");
        return;
    }
    auth_key_id(id);
    if (auth_paired()) reply_ok("paired=on id=%s", id);
    else reply_ok("paired=off");
}

/* BROWSER                    -> OK BROWSER <id> <redirect> <url> <title> | OK BROWSER none
   BROWSER <id> <query>       answer a pending login (the redirect's query string)
   BROWSER <id> CANCEL        decline it */
static void cmd_browser(char *args)
{
    char *id = next_word(&args);
    if (!*id) {
        static char buf[PROTO_MSG_MAX - 16];
        if (browser_pending(buf, sizeof buf)) reply_ok("BROWSER %s", buf);
        else reply_ok("BROWSER none");
        return;
    }
    char *q = next_word(&args);
    if (!*q) { reply_err("usage: BROWSER <id> <query>|CANCEL"); return; }
    if (browser_answer(atoi(id), q)) reply_ok("BROWSER %s", !strcmp(q, "CANCEL") ? "cancelled" : "answered");
    else reply_err("no pending browser request %s", id);
}

static void list_secret(const char *app, const char *name, size_t len, void *ctx)
{
    (void)ctx;
    char buf[80];
    snprintf(buf, sizeof buf, "%s %s %zu", app, name, len);
    send_line("SECRET", buf);
}

/* Each secret belongs to an app.
   SECRETS [<app>]                  -> SECRET <app> <name> <bytes> lines, OK <count> (never values)
   SECRET SET <app> <name> <hex>    store (hex so any bytes fit on one line)
   SECRET PART <app> <name> <hex>   the start of a value too long for one line:
                                    the next SET of that name from this session
                                    adds the rest
   SECRET RM <app> <name>           delete one
   SECRET CLEAR <app>               delete all of an app's secrets */

static void part_drop(session_t *s)
{
    if (s->part.buf) memset(s->part.buf, 0, s->part.len);   /* a secret: don't leave it in the heap */
    free(s->part.buf);
    memset(&s->part, 0, sizeof s->part);
}

static void cmd_secrets(char *args)
{
    char *app = next_word(&args);
    reply_ok("%d", secrets_list(*app ? app : NULL, list_secret, NULL));
}

static void cmd_secret(char *args)
{
    char *sub = next_word(&args), *app = next_word(&args);
    if (!vfs_valid_app(app)) { reply_err("app names are 1..23 characters: A-Z a-z 0-9 _ . -"); return; }
    if (!strcmp(sub, "CLEAR")) {
        esp_err_t e = secrets_clear(app);
        if (e == ESP_OK) reply_ok("SECRET CLEAR %s", app);
        else reply_err("%s", esp_err_to_name(e));
        return;
    }
    char *name = next_word(&args);
    if (!secrets_valid_name(name)) { reply_err("secret names are 1..15 characters: A-Z a-z 0-9 _ . -"); return; }
    esp_err_t err;
    if (!strcmp(sub, "RM")) {
        err = secrets_set(app, name, NULL, 0);
    } else if (!strcmp(sub, "SET") || !strcmp(sub, "PART")) {
        char *hex = next_word(&args);
        long n = hex_decode(hex, hex, strlen(hex) / 2);   /* in place */
        if (n < 0) { reply_err("value must be hex"); return; }
        secret_part_t *part = &s_cur->part;
        bool more = part->buf && !strcmp(part->app, app) && !strcmp(part->name, name);
        bool is_part = !strcmp(sub, "PART");
        if (!more) part_drop(s_cur);
        if (is_part && !part->buf) {
            part->buf = malloc(SECRET_MAX_LEN);
            if (!part->buf) { reply_err("out of memory"); return; }
            snprintf(part->app, sizeof part->app, "%s", app);
            snprintf(part->name, sizeof part->name, "%s", name);
        }
        if (part->buf) {                                    /* PART, or the SET that ends them */
            if ((size_t)n > SECRET_MAX_LEN - part->len) {
                part_drop(s_cur);
                reply_err("value too long (max %d bytes)", SECRET_MAX_LEN);
                return;
            }
            memcpy(part->buf + part->len, hex, (size_t)n);
            part->len += (size_t)n;
        }
        if (is_part) {
            reply_ok("SECRET PART %s %s %u", app, name, (unsigned)part->len);
            return;
        }
        err = part->buf ? secrets_set(app, name, part->buf, part->len) : secrets_set(app, name, hex, (size_t)n);
        part_drop(s_cur);
    } else {
        reply_err("usage: SECRET SET|PART <app> <name> <hex> | SECRET RM <app> <name> | SECRET CLEAR <app>");
        return;
    }
    if (err == ESP_OK) reply_ok("SECRET %s %s %s", sub, app, name);
    else if (err == ESP_ERR_NO_MEM) reply_err("too many secrets (max %d on the device)", SECRET_MAX_COUNT);
    else if (err == ESP_ERR_NVS_NOT_ENOUGH_SPACE)
        reply_err("no room: an app's secrets can take %d bytes, all apps' %d", SECRETS_APP_BYTES, SECRETS_TOTAL_BYTES);
    else reply_err("%s", esp_err_to_name(err));
}

/* TIME -> OK synced=0|1 utc=<seconds|-> local=<ISO 8601|-> tz=<POSIX TZ> source=ntp|rtc|- last_sync=<utc|-> syncs=<n>
   TIME SYNC -> OK SYNCING: ask the time server now (EVT time synced... the first time) */
static void cmd_time(char *args)
{
    char *sub = next_word(&args);
    if (!strcasecmp(sub, "SYNC")) {
        if (clock_sync_now() == ESP_OK) reply_ok("SYNCING");
        else reply_err("no network: the time comes from NTP once Wi-Fi is up");
        return;
    }
    int64_t t = clock_now(), last = clock_last_sync();
    char local[40], utc[24] = "-", ls[24] = "-";
    clock_iso_local(t, local, sizeof local);
    if (t) snprintf(utc, sizeof utc, "%lld", (long long)t);
    if (last) snprintf(ls, sizeof ls, "%lld", (long long)last);
    reply_ok("synced=%d utc=%s local=%s tz=%s source=%s last_sync=%s syncs=%u", clock_valid(), utc, local,
             clock_tz(), clock_source(), ls, clock_syncs());
}

/* TZ [<posix tz>] -> OK tz=<posix tz> */
static void cmd_tz(char *args)
{
    char *tz = next_word(&args);
    if (*tz && clock_set_tz(tz) != ESP_OK) {
        reply_err("not a POSIX TZ string (e.g. EET-2EEST,M3.5.0/3,M10.5.0/4 or UTC0): %s", tz);
        return;
    }
    reply_ok("tz=%s", clock_tz());
}

/* The USB tether (tun.c): the session that turned it on gets its frames. */
static session_t *s_tun;

static bool tun_out(const char *line, size_t len) { return sess_write(s_tun, line, len); }

/* TUN ON <utc> | TUN OFF -> OK TUN ON [started] | OK TUN OFF
   TUN OPENED|DATA|ACK|CLOSE <id> ...: the computer's frames, no reply */
static void cmd_tun(char *args)
{
    size_t n = strcspn(args, " ");               /* (a frame goes on whole) */
    bool on = n == 2 && !strncasecmp(args, "ON", 2);
    if (on || (n == 3 && !strncasecmp(args, "OFF", 3))) {
        char *rest = args;
        next_word(&rest);
        if (!s_cur->link.trusted) { reply_err("the tether works only over USB"); return; }
        if (!on) {
            tun_off();
            reply_ok("TUN OFF");
            return;
        }
        char *utc_s = next_word(&rest), *end;
        long long utc = strtoll(utc_s, &end, 10);
        if (!*utc_s || *end || *rest) { reply_err("usage: TUN ON <utc> | TUN OFF"); return; }
        if (s_tun != s_cur) tun_off();           /* (another link had it) */
        s_tun = s_cur;
        bool starts = tun_on(hw_millis());
        if (starts) clock_set_usb(utc);
        reply_ok(starts ? "TUN ON started" : "TUN ON");
        return;
    }
    if (s_cur == s_tun && tun_up()) tun_frame(args);
}

static void cmd_eval(char *code)
{
    char out[256];
    if (script_eval(code, out, sizeof out)) reply_ok("%s", out);
    else reply_err("%s", out);
}

static void cmd_ping(char *args) { reply_ok("PONG"); }

/* RM <app>/<file> */
static void cmd_rm(char *args)
{
    char path[VFS_MAX_NAME];
    /* A bare name resolves to <app>/main.lua; RM must name a file. */
    bool file = strchr(args, '/') || strstr(args, ".lua");
    if (file && script_resolve(args, path, sizeof path) && vfs_delete(path) == ESP_OK)
        reply_ok("REMOVED %s", path);
    else reply_err("no such file: %s", args);
}

/* INSTALL <app>: its files from RAM to flash */
static void cmd_install(char *args)
{
    char *app = next_word(&args), path[VFS_MAX_NAME];
    if (install_has(app)) return;
    esp_err_t err = vfs_install(app);
    if (err == ESP_OK) {
        reply_ok("INSTALLED %s", app);
        proto_eventf("installed %s", app);
        if (app_on_screen(app, path, sizeof path)) restart(path);
    } else {
        reply_err("install %s: %s", app, store_err(err));
    }
}

/* An app install from the marketplace has this app (NULL: any app): the
   commands that would change its files under it wait. */
static bool install_has(const char *app)
{
    if (!app_install_busy()) return false;
    if (app && strcmp(app, app_install_app())) return false;
    reply_err("%s is being installed from the marketplace: try again in a moment", app_install_app());
    return true;
}

/* APPINSTALL <app>: install or update it from the marketplace; the outcome
   comes later as EVT app install <app> ok|failed <why> */
static void cmd_appinstall(char *args)
{
    char *app = next_word(&args), err[64];
    if (menu_installing()) { reply_err("appinstall %s: the menu is installing apps", app); return; }
    if (app_install_start(app, err, sizeof err)) reply_ok("APPINSTALL %s", app);
    else reply_err("appinstall %s: %s", app, err);
}

/* UNINSTALL <app>: its files, data and secrets */
static void cmd_uninstall(char *args)
{
    char *app = next_word(&args);
    if (install_has(app)) return;
    esp_err_t err = app_remove(app);
    if (err == ESP_OK) {
        reply_ok("UNINSTALLED %s", app);
        proto_eventf("uninstalled %s", app);
    } else {
        reply_err("uninstall %s: %s", app, store_err(err));
    }
}

static void cmd_stop(char *args)
{
    bool was = script_running();
    script_stop();
    reply_ok("STOPPED");
    if (was) proto_eventf("stopped");
}

/* SAVE: INSTALL every app that has uploads in RAM */
static void cmd_save(char *args)
{
    int n = 0;
    char path[VFS_MAX_NAME], app[VFS_APP_NAME_MAX];
    vfs_app_state_t st;
    bool shown = app_on_screen(NULL, path, sizeof path), had_uploads = false;
    if (shown) {                                  /* restart it only if SAVE changed it */
        script_app(app, sizeof app);
        had_uploads = vfs_app_state(app, &st) && st != VFS_INSTALLED;
    }
    esp_err_t err = vfs_install_all(&n);
    if (err == ESP_OK) {
        reply_ok("SAVED %d apps", n);
        if (shown && had_uploads) restart(path);
    } else {
        reply_err("save failed: %s", store_err(err));
    }
}

/* FORMAT: every app, its data and secrets, and the autostart setting */
static void cmd_format(char *args)
{
    if (install_has(NULL)) return;
    esp_err_t err = apps_wipe();
    if (err == ESP_OK) reply_ok("FORMATTED");
    else reply_err("format failed: %s", esp_err_to_name(err));
}

/* AUTORUN [<app>|off] -> OK AUTORUN <app>|off */
static void cmd_autorun(char *args)
{
    char *v = next_word(&args);
    esp_err_t err = ESP_OK;
    if (!strcmp(v, "off") || !strcmp(v, "-")) err = settings_set_autostart("");
    else if (*v) {
        vfs_app_state_t st;
        if (!vfs_app_state(v, &st)) { reply_err("no app '%s' (AUTORUN <app> | off)", v); return; }
        if (st == VFS_RAM) { reply_err("'%s' isn't installed: INSTALL it first", v); return; }
        err = settings_set_autostart(v);
    }
    if (err != ESP_OK) { reply_err("%s", esp_err_to_name(err)); return; }
    reply_ok("AUTORUN %s", settings_autostart()[0] ? settings_autostart() : "off");
}

static void cmd_help(char *args);

/* The commands, in HELP's order. during_ota: allowed while a firmware update
   runs, which stopped the running app and owns the screen until it reboots:
   only what doesn't touch apps or settings. */
static const struct {
    const char *name;
    void (*run)(char *args);
    bool during_ota;
} s_commands[] = {
    { "PING", cmd_ping, true },
    { "INFO", cmd_info, true },
    { "LS", cmd_ls, true },
    { "APPS", cmd_apps, true },
    { "PUT", cmd_put, false },
    { "OTA", cmd_ota, true },
    { "GET", cmd_get, true },
    { "FB", cmd_fb, true },
    { "TOUCH", cmd_touch, false },
    { "KEY", cmd_key, false },
    { "RM", cmd_rm, false },
    { "RUN", cmd_run, false },
    { "STOP", cmd_stop, false },
    { "INSTALL", cmd_install, false },
    { "UNINSTALL", cmd_uninstall, false },
    { "APPINSTALL", cmd_appinstall, false },
    { "SAVE", cmd_save, false },
    { "FORMAT", cmd_format, false },
    { "AUTORUN", cmd_autorun, false },
    { "EVAL", cmd_eval, false },
    { "TIME", cmd_time, true },
    { "TZ", cmd_tz, false },
    { "WIFI", cmd_wifi, false },
    { "PAIR", cmd_pair, false },
    { "BROWSER", cmd_browser, false },
    { "SECRETS", cmd_secrets, false },
    { "SECRET", cmd_secret, false },
    { "CRASH", cmd_crash, true },
    { "TUN", cmd_tun, true },
    { "HELP", cmd_help, true },
};
#define N_COMMANDS (sizeof s_commands / sizeof *s_commands)

static void cmd_help(char *args)
{
    char list[256];
    size_t len = 0;
    for (size_t i = 0; i < N_COMMANDS && len < sizeof list; i++)
        len += (size_t)snprintf(list + len, sizeof list - len, "%s ", s_commands[i].name);
    reply_ok("%s- see docs/PROTOCOL.md", list);
}

static void handle_line(char *line)
{
    char *args = line;
    char *cmd = next_word(&args);
    for (char *c = cmd; *c; c++) if (*c >= 'a' && *c <= 'z') *c -= 32;

    if (!*cmd) return;
    if (!session_open(s_cur)) {          /* (a Wi-Fi link passes lines on only after login) */
        reply_err("not logged in: pair this computer over USB (luaboard pair)");
        return;
    }
    for (size_t i = 0; i < N_COMMANDS; i++) {
        if (strcmp(cmd, s_commands[i].name)) continue;
        if (ota_active() && !s_commands[i].during_ota) reply_err("a firmware update is running");
        else s_commands[i].run(args);
        return;
    }
    reply_err("unknown command '%s' (try HELP)", cmd);
}

/* --------------------------------------------------------------- poll */

void proto_init(void)
{
    memset(s_sessions, 0, sizeof s_sessions);
    tun_init(&(tun_io_t){ .out = tun_out, .alloc = hw_big_alloc, .free = free, .first_id = esp_random() });
}

int proto_add_link(const proto_link_t *link)
{
    for (int i = 0; i < PROTO_MAX_LINKS; i++) {
        if (!s_sessions[i].used) {
            memset(&s_sessions[i], 0, sizeof s_sessions[i]);
            s_sessions[i].link = *link;
            s_sessions[i].used = true;
            return i;
        }
    }
    return -1;
}

bool proto_link_authed(int id)
{
    return id >= 0 && id < PROTO_MAX_LINKS && s_sessions[id].used && session_open(&s_sessions[id]) &&
           !s_sessions[id].link.trusted;
}

/* End an upload or firmware update in progress (and pieces of a secret). */
static void transfer_drop(session_t *s, const char *why)
{
    if (s->state == ST_OTA) ota_abort(why);
    free(s->up.buf);
    s->up.buf = NULL;
    part_drop(s);
    s->state = ST_LINE;
}

void proto_link_reset(int id)
{
    if (id < 0 || id >= PROTO_MAX_LINKS) return;
    session_t *s = &s_sessions[id];
    if (s_touch_link == id) { buttons_remote(false, 0, 0); s_touch_link = -1; }   /* its finger goes with it */
    transfer_drop(s, "the link went away");
    s->line_len = 0;
    s->line_overflow = false;
    s->authed = false;          /* a new peer must log in again */
}

void proto_link_login(int id)
{
    if (id >= 0 && id < PROTO_MAX_LINKS && s_sessions[id].used) s_sessions[id].authed = true;
}

static void poll_session(session_t *s)
{
    uint8_t buf[256];
    int n;
    /* A firmware image is written to flash as it comes, which takes time:
       hand the main loop back now and then (watchdog, screen). */
    size_t budget = 16 * 1024;
    s_cur = s;
    while (budget > 0 && (n = s->link.read(s->link.ctx, buf, sizeof buf)) > 0) {
        if (s->state == ST_OTA || s->state == ST_DRAIN) budget = (size_t)n < budget ? budget - (size_t)n : 0;
        if (s->state == ST_DRAIN) { s->up.last_ms = hw_millis(); continue; }
        for (int i = 0; i < n; i++) {
            uint8_t c = buf[i];
            if (s->state == ST_OTA) {                    /* (ota.c counts the bytes) */
                size_t took = ota_write(buf + i, (size_t)(n - i)), size;
                if (!took) { s->state = ST_LINE; i--; continue; }   /* no update running after all */
                i += (int)took - 1;
                s->up.last_ms = hw_millis();
                if (ota_received(&size) == size) ota_upload_finish();
                continue;
            }
            if (s->state == ST_UPLOAD) {
                /* Bulk copy as much of the payload as this chunk holds. */
                size_t take = s->up.size - s->up.got;
                if (take > (size_t)(n - i)) take = (size_t)(n - i);
                memcpy(s->up.buf + s->up.got, buf + i, take);
                s->up.got += take;
                i += (int)take - 1;
                s->up.last_ms = hw_millis();
                if (s->up.got == s->up.size) upload_finish();
                continue;
            }
            if (c == '\r') continue;
            if (c == '\n') {
                if (s->line_overflow) reply_err("line too long (max %d)", PROTO_LINE_MAX - 1);
                else { s->line[s->line_len] = 0; handle_line(s->line); }
                if (s == s_tun) tun_heard(hw_millis());     /* (once done: a long command doesn't count) */
                s->line_len = 0;
                s->line_overflow = false;
                if (s->state == ST_UPLOAD && s->up.size == 0) upload_finish();
                continue;
            }
            if (s->line_len < sizeof s->line - 1) s->line[s->line_len++] = (char)c;
            else s->line_overflow = true;
        }
    }
    if (s->state == ST_OTA && hw_millis() - s->up.last_ms > PROTO_UPLOAD_TIMEOUT_MS) {
        size_t size, got = ota_received(&size);
        ESP_LOGW(TAG, "%s: firmware upload timed out at %zu/%zu", s->link.name, got, size);
        reply_err("upload timeout (%zu/%zu bytes)", got, size);
        transfer_drop(s, "upload timeout");
        s->state = ST_DRAIN;
        s->up.last_ms = hw_millis();
    }
    if (s->state == ST_UPLOAD && hw_millis() - s->up.last_ms > PROTO_UPLOAD_TIMEOUT_MS) {
        ESP_LOGW(TAG, "%s: upload of %s timed out at %zu/%zu", s->link.name, s->up.name, s->up.got, s->up.size);
        reply_err("upload timeout (%zu/%zu bytes)", s->up.got, s->up.size);
        transfer_drop(s, "upload timeout");
        s->state = ST_DRAIN;
        s->up.last_ms = hw_millis();
    }
    if (s->state == ST_DRAIN && hw_millis() - s->up.last_ms > PROTO_DRAIN_QUIET_MS) s->state = ST_LINE;
    s_cur = NULL;
}

void proto_poll(void)
{
    for (int i = 0; i < PROTO_MAX_LINKS; i++)
        if (s_sessions[i].used) poll_session(&s_sessions[i]);
    tun_poll(hw_millis());     /* (after them: lines that came while the loop was busy count) */
}
