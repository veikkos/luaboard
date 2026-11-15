/* Simulated Bluetooth LE peripherals from JSON descriptions. See ble_device.h. */
#include "ble_device.h"

#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ble.h"
#include "host/ble_hs.h"
#include "sim_hw.h"

#define MAX_DEVS   8
#define MAX_CHRS   48
#define MAX_SUBMSG 8
#define VAL_MAX    512

/* ------------------------------------------------------------ mini JSON */

typedef enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } jtype_t;
typedef struct jnode {
    jtype_t type;
    double num;
    char *str;               /* J_STR; also the key of a member */
    char *key;
    struct jnode *child, *next;
} jnode_t;

typedef struct { const char *p; const char *err; } jparser_t;

static void jskip(jparser_t *j) { while (isspace((unsigned char)*j->p)) j->p++; }

static jnode_t *jnew(jtype_t t)
{
    jnode_t *n = calloc(1, sizeof *n);
    if (!n) abort();
    n->type = t;
    return n;
}

static char *jstring(jparser_t *j)
{
    if (*j->p != '"') { j->err = "expected a string"; return NULL; }
    j->p++;
    size_t cap = 64, len = 0;
    char *s = malloc(cap);
    while (*j->p && *j->p != '"') {
        char c = *j->p++;
        if (c == '\\') {
            char e = *j->p++;
            c = e == 'n' ? '\n' : e == 't' ? '\t' : e == 'r' ? '\r' : e == 'b' ? '\b' : e == 'f' ? '\f' : e;
            if (e == 'u') {                       /* ASCII only, enough for names */
                unsigned v = 0;
                if (sscanf(j->p, "%4x", &v) != 1) { j->err = "bad \\u escape"; free(s); return NULL; }
                j->p += 4;
                c = v < 0x80 ? (char)v : '?';
            }
        }
        if (len + 2 > cap) s = realloc(s, cap *= 2);
        s[len++] = c;
    }
    if (*j->p != '"') { j->err = "unterminated string"; free(s); return NULL; }
    j->p++;
    s[len] = 0;
    return s;
}

static jnode_t *jvalue(jparser_t *j);

static jnode_t *jlist(jparser_t *j, bool obj)
{
    jnode_t *n = jnew(obj ? J_OBJ : J_ARR), **tail = &n->child;
    j->p++;
    jskip(j);
    if (*j->p == (obj ? '}' : ']')) { j->p++; return n; }
    for (;;) {
        char *key = NULL;
        if (obj) {
            jskip(j);
            if (!(key = jstring(j))) return n;
            jskip(j);
            if (*j->p != ':') { j->err = "expected ':'"; free(key); return n; }
            j->p++;
        }
        jnode_t *v = jvalue(j);
        if (!v) { free(key); return n; }
        v->key = key;
        *tail = v;
        tail = &v->next;
        jskip(j);
        if (*j->p == ',') { j->p++; continue; }
        if (*j->p == (obj ? '}' : ']')) { j->p++; return n; }
        j->err = obj ? "expected ',' or '}'" : "expected ',' or ']'";
        return n;
    }
}

static jnode_t *jvalue(jparser_t *j)
{
    jskip(j);
    if (j->err) return NULL;
    char c = *j->p;
    if (c == '{' || c == '[') {
        jnode_t *n = jlist(j, c == '{');
        return j->err ? NULL : n;
    }
    if (c == '"') {
        char *s = jstring(j);
        if (!s) return NULL;
        jnode_t *n = jnew(J_STR);
        n->str = s;
        return n;
    }
    if (!strncmp(j->p, "true", 4) || !strncmp(j->p, "false", 5)) {
        jnode_t *n = jnew(J_BOOL);
        n->num = c == 't';
        j->p += c == 't' ? 4 : 5;
        return n;
    }
    if (!strncmp(j->p, "null", 4)) { j->p += 4; return jnew(J_NULL); }
    char *end;
    double v = strtod(j->p, &end);
    if (end == j->p) { j->err = "unexpected character"; return NULL; }
    j->p = end;
    jnode_t *n = jnew(J_NUM);
    n->num = v;
    return n;
}

static const jnode_t *jget(const jnode_t *obj, const char *key)
{
    if (!obj || obj->type != J_OBJ) return NULL;
    for (const jnode_t *c = obj->child; c; c = c->next) if (c->key && !strcmp(c->key, key)) return c;
    return NULL;
}

static const char *jstr(const jnode_t *obj, const char *key)
{
    const jnode_t *n = jget(obj, key);
    return n && n->type == J_STR ? n->str : NULL;
}

/* ------------------------------------------------------------- devices */

typedef struct {
    uint8_t len;
    uint8_t data[64];
} submsg_t;              /* len 0 with use_value: the characteristic's value */

typedef struct {
    uint8_t value[VAL_MAX];
    size_t len;
    int nsub;
    struct { bool use_value; submsg_t m; } sub[MAX_SUBMSG];
} chr_state_t;

typedef struct {
    char id[32];
    int sim;             /* sim_ble_add id */
    bool present;
    sim_ble_device_t dev;
    sim_ble_chr_t chrs[MAX_CHRS];
    chr_state_t st[MAX_CHRS];
    uint8_t adv[31];
    pthread_mutex_t lock;
} bdev_t;

static bdev_t *s_devs[MAX_DEVS];
static int s_ndevs;

/* "c0 ff-ee" / "c0ffee" -> bytes; -1 if it isn't hex. */
static int parse_hex(const char *s, uint8_t *out, size_t cap)
{
    size_t n = 0;
    int hi = -1;
    for (; *s; s++) {
        if (isspace((unsigned char)*s) || *s == '-' || *s == ':') continue;
        if (!isxdigit((unsigned char)*s)) return -1;
        int v = isdigit((unsigned char)*s) ? *s - '0' : tolower((unsigned char)*s) - 'a' + 10;
        if (hi < 0) { hi = v; continue; }
        if (n >= cap) return -1;
        out[n++] = (uint8_t)(hi << 4 | v);
        hi = -1;
    }
    return hi >= 0 ? -1 : (int)n;
}

static uint8_t parse_props(const char *s)
{
    static const struct { const char *name; uint8_t bit; } P[] = {
        { "read", BLE_GATT_CHR_PROP_READ }, { "write_nr", BLE_GATT_CHR_PROP_WRITE_NO_RSP },
        { "write", BLE_GATT_CHR_PROP_WRITE }, { "notify", BLE_GATT_CHR_PROP_NOTIFY },
        { "indicate", BLE_GATT_CHR_PROP_INDICATE },
    };
    uint8_t props = 0;
    char word[16];
    while (*s) {
        while (*s && !isalpha((unsigned char)*s) && *s != '_') s++;
        size_t n = 0;
        while ((isalpha((unsigned char)*s) || *s == '_') && n + 1 < sizeof word) word[n++] = *s++;
        word[n] = 0;
        if (!n) break;
        bool known = false;
        for (size_t i = 0; i < sizeof P / sizeof P[0]; i++)
            if (!strcmp(word, P[i].name)) { props |= P[i].bit; known = true; }
        if (!known) return 0xff;
    }
    return props;
}

static int on_read(void *ctx, int chr, uint8_t *buf, size_t cap)
{
    bdev_t *d = ctx;
    pthread_mutex_lock(&d->lock);
    size_t n = d->st[chr].len < cap ? d->st[chr].len : cap;
    memcpy(buf, d->st[chr].value, n);
    pthread_mutex_unlock(&d->lock);
    return (int)n;
}

static int on_write(void *ctx, int chr, const uint8_t *data, size_t len)
{
    bdev_t *d = ctx;                       /* (the mock checks the props first) */
    pthread_mutex_lock(&d->lock);
    d->st[chr].len = len < VAL_MAX ? len : VAL_MAX;
    memcpy(d->st[chr].value, data, d->st[chr].len);
    pthread_mutex_unlock(&d->lock);
    return 0;
}

static void on_subscribe(void *ctx, int chr, bool on)
{
    bdev_t *d = ctx;
    if (!on) return;
    for (int i = 0; i < d->st[chr].nsub; i++) {
        uint8_t buf[VAL_MAX];
        size_t n;
        pthread_mutex_lock(&d->lock);
        if (d->st[chr].sub[i].use_value) { n = d->st[chr].len; memcpy(buf, d->st[chr].value, n); }
        else { n = d->st[chr].sub[i].m.len; memcpy(buf, d->st[chr].sub[i].m.data, n); }
        pthread_mutex_unlock(&d->lock);
        sim_ble_notify(d->sim, chr, buf, n);
    }
}

static bool fail(const char *path, const char *msg, const char *what)
{
    fprintf(stderr, "%s: %s%s%s\n", path, msg, what ? " " : "", what ? what : "");
    return false;
}

bool ble_device_load(const char *path)
{
    if (s_ndevs >= MAX_DEVS) return fail(path, "too many simulated Bluetooth devices", NULL);
    FILE *f = fopen(path, "rb");
    if (!f) return fail(path, "can't open", NULL);
    static char text[64 * 1024];
    size_t n = fread(text, 1, sizeof text - 1, f);
    fclose(f);
    text[n] = 0;
    jparser_t j = { .p = text };
    jnode_t *root = jvalue(&j);
    if (!root || j.err || root->type != J_OBJ) return fail(path, "not a JSON object:", j.err);

    bdev_t *d = calloc(1, sizeof *d);
    pthread_mutex_init(&d->lock, NULL);
    const char *id = jstr(root, "id");
    if (id) snprintf(d->id, sizeof d->id, "%s", id);
    else {                                   /* the file's name, without folders and .json */
        const char *base = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;
        snprintf(d->id, sizeof d->id, "%.*s", (int)strcspn(base, "."), base);
    }
    const char *addr = jstr(root, "addr");
    if (!addr || parse_hex(addr, d->dev.addr, 6) != 6) return fail(path, "needs \"addr\" like", "\"c7:18:2c:51:4d:01\"");
    d->dev.random = (d->dev.addr[0] & 0xc0) == 0xc0;

    const char *name = jstr(root, "name");
    const char *adv = jstr(root, "adv");
    if (adv) {
        int len = parse_hex(adv, d->adv, sizeof d->adv);
        if (len < 0) return fail(path, "\"adv\": hex, at most 31 bytes", NULL);
        d->dev.adv_len = (size_t)len;
    } else {
        size_t len = 0, nl = name ? strlen(name) : 0;
        d->adv[len++] = 2; d->adv[len++] = 0x01; d->adv[len++] = 0x06;      /* flags */
        if (nl > 31 - 5) nl = 31 - 5;
        if (nl) { d->adv[len++] = (uint8_t)(nl + 1); d->adv[len++] = 0x09; memcpy(d->adv + len, name, nl); len += nl; }
        d->dev.adv_len = len;
    }
    d->dev.adv = d->adv;

    const jnode_t *chrs = jget(root, "characteristics");
    if (!chrs || chrs->type != J_ARR) return fail(path, "needs a \"characteristics\" list", NULL);
    int k = 0;
    for (const jnode_t *c = chrs->child; c; c = c->next) {
        if (k == MAX_CHRS) return fail(path, "too many characteristics", NULL);
        const char *svc = jstr(c, "service"), *uuid = jstr(c, "uuid"), *props = jstr(c, "props");
        if (!svc || !uuid || !ble_uuid_parse(svc, &d->chrs[k].svc) || !ble_uuid_parse(uuid, &d->chrs[k].uuid))
            return fail(path, "each characteristic needs \"service\" and \"uuid\":", uuid ? uuid : "?");
        uint8_t p = props ? parse_props(props) : 0;
        if (p == 0xff) return fail(path, "props: read, write, write_nr, notify, indicate, not", props);
        d->chrs[k].props = p;
        const jnode_t *pair = jget(c, "pairing");
        d->chrs[k].flags = pair && pair->type == J_BOOL && pair->num ? SIM_BLE_ENCRYPTED : 0;
        const char *value = jstr(c, "value"), *txt = jstr(c, "text");
        if (value) {
            int len = parse_hex(value, d->st[k].value, VAL_MAX);
            if (len < 0) return fail(path, "\"value\" isn't hex:", value);
            d->st[k].len = (size_t)len;
        } else if (txt) {
            d->st[k].len = strlen(txt) < VAL_MAX ? strlen(txt) : VAL_MAX;
            memcpy(d->st[k].value, txt, d->st[k].len);
        }
        const jnode_t *subs = jget(c, "on_subscribe");
        for (const jnode_t *s = subs && subs->type == J_ARR ? subs->child : NULL; s && d->st[k].nsub < MAX_SUBMSG; s = s->next) {
            if (s->type != J_STR) continue;
            int i = d->st[k].nsub++;
            if (!strcmp(s->str, "value")) { d->st[k].sub[i].use_value = true; continue; }
            int len = parse_hex(s->str, d->st[k].sub[i].m.data, sizeof d->st[k].sub[i].m.data);
            if (len < 0) return fail(path, "on_subscribe: hex (at most 64 bytes) or \"value\", not", s->str);
            d->st[k].sub[i].m.len = (uint8_t)len;
        }
        k++;
    }
    d->dev.chrs = d->chrs;
    d->dev.nchrs = k;
    d->dev.on_read = on_read;
    d->dev.on_write = on_write;
    d->dev.on_subscribe = on_subscribe;
    d->dev.ctx = d;
    d->sim = sim_ble_add(&d->dev);
    if (d->sim < 0) return fail(path, "rejected (too many devices or characteristics)", NULL);
    const jnode_t *rssi = jget(root, "rssi");
    if (rssi && rssi->type == J_NUM) sim_ble_set_rssi(d->sim, (int)rssi->num);
    const jnode_t *present = jget(root, "present");
    d->present = !present || present->type != J_BOOL || present->num;
    sim_ble_set_present(d->sim, d->present);
    s_devs[s_ndevs++] = d;
    return true;
}

/* ------------------------------------------------------------- controls */

static bdev_t *dev_by_id(const char *id, size_t n)
{
    for (int i = 0; i < s_ndevs; i++)
        if (strlen(s_devs[i]->id) == n && !strncmp(s_devs[i]->id, id, n)) return s_devs[i];
    return NULL;
}

static int chr_by_uuid(bdev_t *d, const char *uuid, size_t n)
{
    char u[48];
    ble_uuid_any_t want;
    if (n >= sizeof u) return -1;
    memcpy(u, uuid, n);
    u[n] = 0;
    if (!ble_uuid_parse(u, &want)) return -1;
    for (int i = 0; i < d->dev.nchrs; i++) if (!ble_uuid_cmp(&d->chrs[i].uuid.u, &want.u)) return i;
    return -1;
}

bool ble_device_set(const char *key, const char *val)
{
    const char *c1 = strchr(val, ':');
    if (!c1) return false;
    bdev_t *d = dev_by_id(val, (size_t)(c1 - val));
    if (!d) return false;
    const char *rest = c1 + 1;
    if (!strcmp(key, "ble")) {
        if (strcmp(rest, "on") && strcmp(rest, "off")) return false;
        d->present = !strcmp(rest, "on");
        sim_ble_set_present(d->sim, d->present);
        return true;
    }
    if (!strcmp(key, "ble_rssi")) {
        char *end;
        long v = strtol(rest, &end, 10);
        if (!*rest || *end || v < -127 || v > 20) return false;
        sim_ble_set_rssi(d->sim, (int)v);
        return true;
    }
    if (strcmp(key, "ble_value") && strcmp(key, "ble_notify")) return false;
    const char *c2 = strchr(rest, ':');
    if (!c2) return false;
    int chr = chr_by_uuid(d, rest, (size_t)(c2 - rest));
    uint8_t buf[VAL_MAX];
    int len = parse_hex(c2 + 1, buf, sizeof buf);
    if (chr < 0 || len < 0) return false;
    if (!strcmp(key, "ble_value")) on_write(d, chr, buf, (size_t)len);
    sim_ble_notify(d->sim, chr, buf, (size_t)len);
    return true;
}

const char *ble_device_set_help(void)
{
    return "ble=ID:on|off ble_value=ID:UUID:HEX ble_notify=ID:UUID:HEX ble_rssi=ID:DBM";
}

void ble_device_toggle_all(void)
{
    bool any = false;
    for (int i = 0; i < s_ndevs; i++) any = any || s_devs[i]->present;
    for (int i = 0; i < s_ndevs; i++) {
        s_devs[i]->present = !any;
        sim_ble_set_present(s_devs[i]->sim, !any);
    }
}
