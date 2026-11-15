/*
 * Bluetooth LE central: NimBLE host plumbing for the Lua `ble` module.
 *
 * Two threads touch this state: the main loop (Lua calls, ble_poll) and the
 * NimBLE host task (GAP/GATT callbacks). Everything shared is under s_mu, and
 * neither side holds s_mu while calling into NimBLE, so the two locks never
 * nest.
 *
 * The stack starts the first time a script uses Bluetooth and then keeps
 * running (idle, it doesn't use the radio). Connecting runs through
 * discovery on the host task (MTU, services, characteristics, the CCCD
 * descriptors of those that can notify) before a connection reports
 * "connected", so scripts can name characteristics by UUID right away.
 *
 * NimBLE runs one connection attempt at a time and can't connect while
 * scanning: ble_poll() queues attempts and pauses the scan meanwhile.
 *
 * A request refused for lack of pairing ("insufficient authentication")
 * starts pairing ("just works", bonded) and is sent again once the link is
 * encrypted, so devices that want a bond work without the script asking.
 *
 * A bond belongs to the app whose connection made it (s_owner, in NVS).
 * Another app can't connect to that device, since it would get the
 * encrypted link without pairing; uninstalling the app or clearing its
 * data deletes its bonds; and when the store is full, a new bond only
 * pushes out an older one of the same app (or one nobody owns).
 */
#include "ble.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_config.h"
#include "esp_log.h"
#include "hex.h"
#include "hw.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "settings.h"

void ble_store_config_init(void);

static const char *TAG = "ble";

#define MAX_ADV      32      /* devices remembered from scanning */
#define MAX_SVCS     16
#define MAX_CHRS     48
#define NOTE_N       16      /* notifications queued per connection */
#define NOTE_MAX     244     /* MTU 247 - 3 */
#define MAX_OPS      8
#define OP_TIMEOUT_MS   30000
#define CONNECT_TIMEOUT_MS 15000
#define SCAN_ITVL    160     /* 100 ms (units of 0.625 ms) ... */
#define SCAN_WINDOW  48      /* ... listening 30 ms of it: leaves the radio to Wi-Fi the rest */

typedef struct {
    bool used, fresh;
    ble_addr_t addr;
    int rssi;
    uint8_t adv[31], rsp[31];
    uint8_t adv_len, rsp_len;
} adv_t;

typedef enum { C_FREE, C_QUEUED, C_CONNECTING, C_DISCOVERING, C_READY, C_CLOSED } cstate_t;

typedef struct {
    cstate_t state;
    int gen;                 /* changes when the slot is reused: stale callbacks are ignored */
    ble_addr_t addr;
    char app[VFS_APP_NAME_MAX];   /* the app that asked for the connection */
    uint16_t handle;
    bool securing;
    char err[64];
    int nsvc;
    struct { ble_uuid_any_t uuid; uint16_t start, end; } svc[MAX_SVCS];
    int nchr;
    struct { int svc; ble_uuid_any_t uuid; uint16_t def, val, cccd; uint8_t props; } chr[MAX_CHRS];
    int disc_svc, disc_chr;  /* discovery progress */
    int note_head, note_count;
    struct { int chr; uint16_t len; uint8_t data[NOTE_MAX]; } note[NOTE_N];
} conn_t;

typedef enum { O_FREE, O_PENDING, O_WAIT_SEC, O_DONE, O_ERR } ostate_t;
typedef enum { K_READ, K_WRITE, K_SUB } okind_t;

typedef struct {
    ostate_t state;
    int token;
    int slot, gen;
    okind_t kind;
    int chr;
    bool response, retried;
    uint16_t cccd;           /* K_SUB: the value to write */
    uint32_t deadline;
    size_t len;
    uint8_t data[BLE_VALUE_MAX];   /* the value written, then the value read */
    char err[64];
} op_t;

static pthread_mutex_t s_mu = PTHREAD_MUTEX_INITIALIZER;
static bool s_started;
static volatile bool s_synced;
static uint8_t s_own_addr_type;

static adv_t s_adv[MAX_ADV];
static int s_adv_next;
static bool s_want_scan, s_scan_active, s_scanning;

static conn_t *s_conn;       /* [BLE_MAX_CONNS], allocated on first use */
static bool s_connect_busy;  /* a ble_gap_connect() is outstanding */
static op_t *s_ops;          /* [MAX_OPS] */
static int s_next_token = 1;

/* Connection ids given out carry the slot's generation: (gen << 2) | slot. */
static int conn_id(int slot) { return s_conn[slot].gen << 2 | slot; }

/* Under s_mu: the slot an id or callback arg refers to, if still current. */
static int slot_of(int id)
{
    int slot = id & 3;
    if (!s_conn || slot >= BLE_MAX_CONNS || s_conn[slot].gen != id >> 2 || s_conn[slot].state == C_FREE) return -1;
    return slot;
}

/* ---------------------------------------------------------------- errors */

static void err_text(int status, char *out, size_t cap)
{
    if (status > BLE_HS_ERR_ATT_BASE && status < BLE_HS_ERR_HCI_BASE) {
        int att = status - BLE_HS_ERR_ATT_BASE;
        switch (att) {
        case BLE_ATT_ERR_READ_NOT_PERMITTED: snprintf(out, cap, "not readable"); return;
        case BLE_ATT_ERR_WRITE_NOT_PERMITTED: snprintf(out, cap, "not writable"); return;
        case BLE_ATT_ERR_INSUFFICIENT_AUTHEN:
        case BLE_ATT_ERR_INSUFFICIENT_ENC: snprintf(out, cap, "the device refused: pairing needed"); return;
        case BLE_ATT_ERR_INVALID_HANDLE: snprintf(out, cap, "no such attribute"); return;
        case BLE_ATT_ERR_REQ_NOT_SUPPORTED: snprintf(out, cap, "not supported by the device"); return;
        default: snprintf(out, cap, "the device refused (ATT error 0x%02x)", att); return;
        }
    }
    switch (status) {
    case BLE_HS_ENOTCONN: snprintf(out, cap, "not connected"); return;
    case BLE_HS_ETIMEOUT: snprintf(out, cap, "timeout"); return;
    case BLE_HS_ENOMEM: snprintf(out, cap, "out of resources"); return;
    case BLE_HS_EBUSY: snprintf(out, cap, "busy"); return;
    case BLE_HS_EAPP: snprintf(out, cap, "cancelled"); return;
    default: snprintf(out, cap, "error %d", status); return;
    }
}

static void disconnect_text(int reason, char *out, size_t cap)
{
    if (reason == BLE_HS_HCI_ERR(BLE_ERR_REM_USER_CONN_TERM)) snprintf(out, cap, "disconnected by the device");
    else if (reason == BLE_HS_HCI_ERR(BLE_ERR_CONN_SPVN_TMO)) snprintf(out, cap, "connection lost");
    else if (reason == BLE_HS_HCI_ERR(BLE_ERR_CONN_TERM_LOCAL)) snprintf(out, cap, "closed");
    else if (reason == BLE_HS_HCI_ERR(BLE_ERR_CONN_ESTABLISHMENT)) snprintf(out, cap, "cannot connect");
    else snprintf(out, cap, "disconnected (reason 0x%x)", reason);
}

/* --------------------------------------------------- bonds and apps */

#define MAX_OWNERS   16      /* more than CONFIG_BT_NIMBLE_MAX_BONDS */
#define OWNER_REC    (1 + 6 + VFS_APP_NAME_MAX)   /* in NVS: type, address, app */
#define NVS_KEY_BLE_OWNERS "bleowners"

typedef struct {
    ble_addr_t addr;
    char app[VFS_APP_NAME_MAX];   /* "": the app is gone, delete the bond when the stack runs */
} owner_t;

/* Its own lock: taken from both threads, never while calling into NimBLE
   or holding s_mu. */
static pthread_mutex_t s_own_mu = PTHREAD_MUTEX_INITIALIZER;
static owner_t s_owner[MAX_OWNERS];
static int s_nowner = -1;            /* -1: not read from NVS yet */

static bool same_addr(const ble_addr_t *a, const ble_addr_t *b) { return !memcmp(a->val, b->val, 6); }

static void owners_load_locked(void)
{
    if (s_nowner >= 0) return;
    s_nowner = 0;
    uint8_t buf[MAX_OWNERS * OWNER_REC];
    size_t len = settings_load(NVS_KEY_BLE_OWNERS, buf, sizeof buf);
    for (size_t o = 0; o + OWNER_REC <= len && s_nowner < MAX_OWNERS; o += OWNER_REC) {
        owner_t *w = &s_owner[s_nowner++];
        w->addr.type = buf[o];
        memcpy(w->addr.val, buf + o + 1, 6);
        snprintf(w->app, sizeof w->app, "%.*s", VFS_APP_NAME_MAX - 1, (const char *)buf + o + 7);
    }
}

static void owners_save_locked(void)
{
    uint8_t buf[MAX_OWNERS * OWNER_REC];
    memset(buf, 0, sizeof buf);
    for (int i = 0; i < s_nowner; i++) {
        uint8_t *r = buf + i * OWNER_REC;
        r[0] = s_owner[i].addr.type;
        memcpy(r + 1, s_owner[i].addr.val, 6);
        memcpy(r + 7, s_owner[i].app, strlen(s_owner[i].app));
    }
    settings_store(NVS_KEY_BLE_OWNERS, buf, (size_t)s_nowner * OWNER_REC);
}

static int owner_index_locked(const ble_addr_t *a)
{
    owners_load_locked();
    for (int i = 0; i < s_nowner; i++) if (same_addr(&s_owner[i].addr, a)) return i;
    return -1;
}

static void owner_del_locked(int i)
{
    s_owner[i] = s_owner[--s_nowner];
    owners_save_locked();
}

/* The app owning the bond with a, into out ("" if none). */
static bool owner_get(const ble_addr_t *a, char *out, size_t cap)
{
    pthread_mutex_lock(&s_own_mu);
    int i = owner_index_locked(a);
    if (i >= 0) snprintf(out, cap, "%s", s_owner[i].app);
    pthread_mutex_unlock(&s_own_mu);
    return i >= 0;
}

static void owner_set(const ble_addr_t *a, const char *app)
{
    pthread_mutex_lock(&s_own_mu);
    int i = owner_index_locked(a);
    if (i < 0 && s_nowner < MAX_OWNERS) i = s_nowner++;
    if (i >= 0 && strcmp(s_owner[i].app, app)) {
        s_owner[i].addr = *a;
        snprintf(s_owner[i].app, sizeof s_owner[i].app, "%s", app);
        owners_save_locked();
    }
    pthread_mutex_unlock(&s_own_mu);
}

static void owner_forget(const ble_addr_t *a)
{
    pthread_mutex_lock(&s_own_mu);
    int i = owner_index_locked(a);
    if (i >= 0) owner_del_locked(i);
    pthread_mutex_unlock(&s_own_mu);
}

static bool is_bonded(const ble_addr_t *a)
{
    ble_addr_t peers[MAX_OWNERS];
    int n = 0;
    if (ble_store_util_bonded_peers(peers, &n, MAX_OWNERS) != 0) return false;
    for (int i = 0; i < n; i++) if (same_addr(&peers[i], a)) return true;
    return false;
}

/* Bonds of apps that are gone: delete them (with the stack running). */
static void forget_orphans(void)
{
    ble_addr_t gone[MAX_OWNERS];
    int n = 0;
    pthread_mutex_lock(&s_own_mu);
    owners_load_locked();
    for (int i = 0; i < s_nowner; )
        if (!s_owner[i].app[0]) { gone[n++] = s_owner[i].addr; s_owner[i] = s_owner[--s_nowner]; }
        else i++;
    if (n) owners_save_locked();
    pthread_mutex_unlock(&s_own_mu);
    for (int i = 0; i < n; i++) ble_gap_unpair(&gone[i]);
}

void ble_forget_app(const char *app)
{
    bool changed = false;
    pthread_mutex_lock(&s_own_mu);
    owners_load_locked();
    for (int i = 0; i < s_nowner; i++)
        if (s_owner[i].app[0] && (!app || !strcmp(s_owner[i].app, app))) { s_owner[i].app[0] = 0; changed = true; }
    if (changed) owners_save_locked();
    pthread_mutex_unlock(&s_own_mu);
    if (changed && s_synced) forget_orphans();   /* else once the stack is up (on_sync) */
}

/* The store is full and NimBLE wants room for a new bond: push out the
   oldest one of the same app, or one nobody owns, never another app's. */
static int on_store_status(struct ble_store_status_event *ev, void *arg)
{
    if (ev->event_code != BLE_STORE_EVENT_OVERFLOW) return ble_store_util_status_rr(ev, arg);
    const ble_addr_t *peer;
    switch (ev->overflow.obj_type) {
    case BLE_STORE_OBJ_TYPE_OUR_SEC:
    case BLE_STORE_OBJ_TYPE_PEER_SEC: peer = &ev->overflow.value->sec.peer_addr; break;
    case BLE_STORE_OBJ_TYPE_CCCD: peer = &ev->overflow.value->cccd.peer_addr; break;
    default: return ble_store_util_status_rr(ev, arg);
    }
    char app[VFS_APP_NAME_MAX] = "";
    pthread_mutex_lock(&s_mu);
    for (int i = 0; s_conn && i < BLE_MAX_CONNS; i++)
        if (s_conn[i].state != C_FREE && s_conn[i].state != C_CLOSED && same_addr(&s_conn[i].addr, peer))
            snprintf(app, sizeof app, "%s", s_conn[i].app);
    pthread_mutex_unlock(&s_mu);

    ble_addr_t peers[MAX_OWNERS];
    int n = 0;
    ble_store_util_bonded_peers(peers, &n, MAX_OWNERS);
    for (int i = 0; i < n; i++) {                        /* oldest first */
        char owner[VFS_APP_NAME_MAX] = "";
        if (same_addr(&peers[i], peer)) continue;
        if (owner_get(&peers[i], owner, sizeof owner) && owner[0] && strcmp(owner, app)) continue;
        ESP_LOGI(TAG, "bond store full: dropping an older bond of %s", owner[0] ? owner : "no app");
        owner_forget(&peers[i]);
        return ble_gap_unpair(&peers[i]);
    }
    ESP_LOGW(TAG, "bond store full of other apps' bonds: not pairing");
    return BLE_HS_ESTORE_CAP;
}

/* ----------------------------------------------------------- the host */

static void on_sync(void)
{
    uint8_t t = BLE_ADDR_PUBLIC;
    if (ble_hs_id_infer_auto(0, &t) != 0) ESP_LOGW(TAG, "no address type");
    s_own_addr_type = t;
    s_synced = true;
    forget_orphans();
}

static void on_reset(int reason)
{
    s_synced = false;
    ESP_LOGW(TAG, "host reset (reason %d)", reason);
}

static void host_task(void *param)
{
    nimble_port_run();               /* returns only after nimble_port_stop() */
    nimble_port_freertos_deinit();
}

static bool start(char *err, size_t errlen)
{
    if (s_started) return true;
    if (!s_conn) s_conn = calloc(BLE_MAX_CONNS, sizeof *s_conn);
    if (!s_ops) s_ops = calloc(MAX_OPS, sizeof *s_ops);
    if (!s_conn || !s_ops) { snprintf(err, errlen, "Bluetooth: out of memory"); return false; }
    esp_err_t rc = nimble_port_init();
    if (rc != 0) {
        snprintf(err, errlen, "Bluetooth didn't start (%d)", (int)rc);
        return false;
    }
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.store_status_cb = on_store_status;
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;   /* "just works" pairing */
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_store_config_init();         /* bonds persist in NVS */
    nimble_port_freertos_init(host_task);
    s_started = true;
    ESP_LOGI(TAG, "started");
    return true;
}

/* -------------------------------------------------------------- scanning */

static void adv_store(const struct ble_gap_disc_desc *d)
{
    pthread_mutex_lock(&s_mu);
    int idx = -1, free_idx = -1;
    for (int i = 0; i < MAX_ADV; i++) {
        if (s_adv[i].used && !memcmp(&s_adv[i].addr, &d->addr, sizeof d->addr)) { idx = i; break; }
        if (!s_adv[i].used && free_idx < 0) free_idx = i;
    }
    if (idx < 0) {
        idx = free_idx >= 0 ? free_idx : s_adv_next;     /* full: replace one in turn */
        memset(&s_adv[idx], 0, sizeof s_adv[idx]);
        s_adv[idx].used = true;
        s_adv[idx].addr = d->addr;
    }
    adv_t *a = &s_adv[idx];
    uint8_t n = d->length_data > 31 ? 31 : d->length_data;
    if (d->event_type == BLE_HCI_ADV_RPT_EVTYPE_SCAN_RSP) { memcpy(a->rsp, d->data, n); a->rsp_len = n; }
    else { memcpy(a->adv, d->data, n); a->adv_len = n; }
    a->rssi = d->rssi;
    a->fresh = true;
    pthread_mutex_unlock(&s_mu);
}

static int scan_event(struct ble_gap_event *ev, void *arg)
{
    if (ev->type == BLE_GAP_EVENT_DISC) adv_store(&ev->disc);
    else if (ev->type == BLE_GAP_EVENT_DISC_COMPLETE) {
        pthread_mutex_lock(&s_mu);
        s_scanning = false;           /* ble_poll() restarts it if still wanted */
        pthread_mutex_unlock(&s_mu);
    }
    return 0;
}

bool ble_scan_start(bool active, char *err, size_t errlen)
{
    if (!start(err, errlen)) return false;
    pthread_mutex_lock(&s_mu);
    bool restart = s_scanning && s_scan_active != active;   /* to change mode: stop, ble_poll starts again */
    if (restart) s_scanning = false;
    s_want_scan = true;
    s_scan_active = active;
    pthread_mutex_unlock(&s_mu);
    if (restart) ble_gap_disc_cancel();
    ble_poll();
    return true;
}

void ble_scan_stop(void)
{
    pthread_mutex_lock(&s_mu);
    s_want_scan = false;
    pthread_mutex_unlock(&s_mu);
    ble_poll();
}

bool ble_adv_next(ble_adv_t *out)
{
    bool found = false;
    pthread_mutex_lock(&s_mu);
    for (int k = 0; k < MAX_ADV && !found; k++) {
        int i = (s_adv_next + k) % MAX_ADV;
        adv_t *a = &s_adv[i];
        if (!a->used || !a->fresh) continue;
        a->fresh = false;
        out->addr = a->addr;
        out->rssi = a->rssi;
        memcpy(out->ad, a->adv, a->adv_len);
        memcpy(out->ad + a->adv_len, a->rsp, a->rsp_len);
        out->ad_len = (size_t)a->adv_len + a->rsp_len;
        s_adv_next = (i + 1) % MAX_ADV;
        found = true;
    }
    pthread_mutex_unlock(&s_mu);
    return found;
}

/* ------------------------------------------------------------ requests */

static op_t *op_by_token(int token)
{
    for (int i = 0; i < MAX_OPS; i++) if (s_ops[i].state != O_FREE && s_ops[i].token == token) return &s_ops[i];
    return NULL;
}

static void op_fail(op_t *op, const char *msg)
{
    op->state = O_ERR;
    snprintf(op->err, sizeof op->err, "%s", msg);
}

/* Fail every request of a connection (it went away). Under s_mu. */
static void ops_fail_conn(int slot, const char *msg)
{
    for (int i = 0; i < MAX_OPS; i++)
        if ((s_ops[i].state == O_PENDING || s_ops[i].state == O_WAIT_SEC) && s_ops[i].slot == slot)
            op_fail(&s_ops[i], msg);
}

static int on_attr(uint16_t handle, const struct ble_gatt_error *error, struct ble_gatt_attr *attr, void *arg);

/* Send a request to the peer. Not under s_mu. */
static void op_issue(int token)
{
    pthread_mutex_lock(&s_mu);
    op_t *op = op_by_token(token);
    int slot = op ? op->slot : -1;
    if (!op || op->state != O_PENDING || s_conn[slot].gen != op->gen || s_conn[slot].state != C_READY) {
        if (op && op->state == O_PENDING) op_fail(op, "not connected");
        pthread_mutex_unlock(&s_mu);
        return;
    }
    conn_t *c = &s_conn[slot];
    uint16_t h = c->handle, val = c->chr[op->chr].val, cccd = c->chr[op->chr].cccd;
    okind_t kind = op->kind;
    bool response = op->response;
    uint8_t buf[BLE_VALUE_MAX];
    size_t len = op->len;
    memcpy(buf, op->data, len);
    uint16_t sub = op->cccd;
    pthread_mutex_unlock(&s_mu);

    void *arg = (void *)(intptr_t)token;
    int rc;
    if (kind == K_READ) rc = ble_gattc_read(h, val, on_attr, arg);
    else if (kind == K_SUB) {
        uint8_t v[2] = { (uint8_t)sub, (uint8_t)(sub >> 8) };
        rc = ble_gattc_write_flat(h, cccd, v, 2, on_attr, arg);
    } else if (response) rc = ble_gattc_write_flat(h, val, buf, (uint16_t)len, on_attr, arg);
    else rc = ble_gattc_write_no_rsp_flat(h, val, buf, (uint16_t)len);

    pthread_mutex_lock(&s_mu);
    op = op_by_token(token);
    if (op && op->state == O_PENDING) {
        if (rc) { char m[64]; err_text(rc, m, sizeof m); op_fail(op, m); }
        else if (kind == K_WRITE && !response) { op->state = O_DONE; op->len = 0; }
    }
    pthread_mutex_unlock(&s_mu);
}

static int on_attr(uint16_t handle, const struct ble_gatt_error *error, struct ble_gatt_attr *attr, void *arg)
{
    int token = (int)(intptr_t)arg;
    uint16_t secure_handle = 0;
    pthread_mutex_lock(&s_mu);
    op_t *op = op_by_token(token);
    if (op && op->state == O_PENDING) {
        int st = error ? error->status : 0;
        if (st == 0) {
            op->len = 0;
            if (op->kind == K_READ && attr && attr->om) {
                uint16_t n = 0;
                ble_hs_mbuf_to_flat(attr->om, op->data, sizeof op->data, &n);
                op->len = n;
            }
            op->state = O_DONE;
        } else if ((st == BLE_HS_ATT_ERR(BLE_ATT_ERR_INSUFFICIENT_AUTHEN) ||
                    st == BLE_HS_ATT_ERR(BLE_ATT_ERR_INSUFFICIENT_ENC)) && !op->retried) {
            /* pair, then try again (see ENC_CHANGE) */
            op->retried = true;
            op->state = O_WAIT_SEC;
            conn_t *c = &s_conn[op->slot];
            if (c->gen == op->gen && !c->securing) { c->securing = true; secure_handle = c->handle; }
        } else {
            char m[64];
            err_text(st, m, sizeof m);
            op_fail(op, m);
        }
    }
    pthread_mutex_unlock(&s_mu);
    if (secure_handle) {
        ESP_LOGI(TAG, "pairing (the device asked for it)");
        int rc = ble_gap_security_initiate(secure_handle);
        if (rc && rc != BLE_HS_EALREADY) ESP_LOGW(TAG, "pairing didn't start: %d", rc);
    }
    return 0;
}

static int new_op(int id, int chr, okind_t kind, char *err, size_t errlen, op_t **out)
{
    int slot = slot_of(id);
    if (slot < 0 || s_conn[slot].state != C_READY) {
        snprintf(err, errlen, slot < 0 || s_conn[slot].state == C_CLOSED ? "not connected" : "still connecting");
        return 0;
    }
    if (chr < 0 || chr >= s_conn[slot].nchr) { snprintf(err, errlen, "no such characteristic"); return 0; }
    for (int i = 0; i < MAX_OPS; i++) {
        op_t *op = &s_ops[i];
        if (op->state != O_FREE) continue;
        memset(op, 0, offsetof(op_t, data));
        op->state = O_PENDING;
        op->token = s_next_token++;
        if (s_next_token <= 0) s_next_token = 1;
        op->slot = slot;
        op->gen = s_conn[slot].gen;
        op->kind = kind;
        op->chr = chr;
        op->deadline = hw_millis() + OP_TIMEOUT_MS;
        *out = op;
        return op->token;
    }
    snprintf(err, errlen, "too many Bluetooth requests at once (at most %d)", MAX_OPS);
    return 0;
}

int ble_read(int id, int chr, char *err, size_t errlen)
{
    pthread_mutex_lock(&s_mu);
    op_t *op = NULL;
    int token = new_op(id, chr, K_READ, err, errlen, &op);
    if (token && !(s_conn[op->slot].chr[chr].props & BLE_GATT_CHR_PROP_READ)) {
        op->state = O_FREE;
        token = 0;
        snprintf(err, errlen, "not readable");
    }
    pthread_mutex_unlock(&s_mu);
    if (token) op_issue(token);
    return token;
}

int ble_write(int id, int chr, const void *data, size_t len, bool response, char *err, size_t errlen)
{
    if (len > NOTE_MAX) { snprintf(err, errlen, "at most %d bytes", NOTE_MAX); return 0; }
    pthread_mutex_lock(&s_mu);
    op_t *op = NULL;
    int token = new_op(id, chr, K_WRITE, err, errlen, &op);
    if (token) {
        uint8_t props = s_conn[op->slot].chr[chr].props;
        if (!(props & (response ? BLE_GATT_CHR_PROP_WRITE : BLE_GATT_CHR_PROP_WRITE_NO_RSP))) {
            op->state = O_FREE;
            token = 0;
            snprintf(err, errlen, "not writable%s", response ? "" : " without response");
        } else {
            op->response = response;
            memcpy(op->data, data, len);
            op->len = len;
        }
    }
    pthread_mutex_unlock(&s_mu);
    if (token) op_issue(token);
    return token;
}

int ble_subscribe(int id, int chr, bool on, char *err, size_t errlen)
{
    pthread_mutex_lock(&s_mu);
    op_t *op = NULL;
    int token = new_op(id, chr, K_SUB, err, errlen, &op);
    if (token) {
        uint8_t props = s_conn[op->slot].chr[chr].props;
        if (!s_conn[op->slot].chr[chr].cccd) {
            op->state = O_FREE;
            token = 0;
            snprintf(err, errlen, "doesn't notify");
        } else {
            op->response = true;
            op->cccd = !on ? 0 : (props & BLE_GATT_CHR_PROP_NOTIFY) ? 1 : 2;   /* notify if it can, else indicate */
        }
    }
    pthread_mutex_unlock(&s_mu);
    if (token) op_issue(token);
    return token;
}

int ble_result(int token, uint8_t *buf, size_t cap, size_t *len, char *err, size_t errlen)
{
    int r;
    pthread_mutex_lock(&s_mu);
    op_t *op = s_ops ? op_by_token(token) : NULL;
    if (!op) { snprintf(err, errlen, "unknown request"); r = -1; }
    else if (op->state == O_DONE) {
        size_t n = op->len < cap ? op->len : cap;
        if (buf && n) memcpy(buf, op->data, n);
        if (len) *len = n;
        op->state = O_FREE;
        r = 1;
    } else if (op->state == O_ERR) {
        snprintf(err, errlen, "%s", op->err);
        op->state = O_FREE;
        r = -1;
    } else r = 0;
    pthread_mutex_unlock(&s_mu);
    return r;
}

/* ------------------------------------------------------------ discovery */

static int conn_event(struct ble_gap_event *ev, void *arg);
static void disc_next_chrs(int id);
static void disc_next_dscs(int id);

/* Discovery failed or the peer misbehaved: give up on the connection. */
static void conn_fail(int id, const char *msg)
{
    uint16_t h = 0;
    pthread_mutex_lock(&s_mu);
    int slot = slot_of(id);
    if (slot >= 0 && s_conn[slot].state != C_CLOSED) {
        conn_t *c = &s_conn[slot];
        h = c->handle;
        c->state = C_CLOSED;
        snprintf(c->err, sizeof c->err, "%s", msg);
        ops_fail_conn(slot, msg);
    }
    pthread_mutex_unlock(&s_mu);
    if (h) ble_gap_terminate(h, BLE_ERR_REM_USER_CONN_TERM);
}

static int on_dsc(uint16_t handle, const struct ble_gatt_error *error, uint16_t chr_val_handle,
                  const struct ble_gatt_dsc *dsc, void *arg)
{
    int id = (int)(intptr_t)arg;
    int st = error ? error->status : 0;
    if (st == 0 && dsc) {
        pthread_mutex_lock(&s_mu);
        int slot = slot_of(id);
        if (slot >= 0 && dsc->uuid.u.type == BLE_UUID_TYPE_16 && dsc->uuid.u16.value == BLE_GATT_DSC_CLT_CFG_UUID16) {
            conn_t *c = &s_conn[slot];
            if (c->disc_chr < c->nchr) c->chr[c->disc_chr].cccd = dsc->handle;
        }
        pthread_mutex_unlock(&s_mu);
    } else if (st == BLE_HS_EDONE) {
        pthread_mutex_lock(&s_mu);
        int slot = slot_of(id);
        if (slot >= 0) s_conn[slot].disc_chr++;
        pthread_mutex_unlock(&s_mu);
        disc_next_dscs(id);
    } else {
        conn_fail(id, "discovery failed");
    }
    return 0;
}

/* Find the CCCD of each characteristic that can notify or indicate. */
static void disc_next_dscs(int id)
{
    uint16_t h = 0, start = 0, end = 0;
    bool ready = false;
    pthread_mutex_lock(&s_mu);
    int slot = slot_of(id);
    if (slot >= 0 && s_conn[slot].state == C_DISCOVERING) {
        conn_t *c = &s_conn[slot];
        while (c->disc_chr < c->nchr &&
               !(c->chr[c->disc_chr].props & (BLE_GATT_CHR_PROP_NOTIFY | BLE_GATT_CHR_PROP_INDICATE)))
            c->disc_chr++;
        if (c->disc_chr >= c->nchr) {
            c->state = C_READY;
            ready = true;
        } else {
            int i = c->disc_chr;
            h = c->handle;
            start = c->chr[i].val;
            end = c->svc[c->chr[i].svc].end;
            if (i + 1 < c->nchr && c->chr[i + 1].svc == c->chr[i].svc) end = (uint16_t)(c->chr[i + 1].def - 1);
        }
    }
    pthread_mutex_unlock(&s_mu);
    if (ready) ESP_LOGI(TAG, "connected, discovery done");
    if (!h) return;
    if (end <= start) {              /* no room for a descriptor */
        pthread_mutex_lock(&s_mu);
        int s2 = slot_of(id);
        if (s2 >= 0) s_conn[s2].disc_chr++;
        pthread_mutex_unlock(&s_mu);
        disc_next_dscs(id);
        return;
    }
    int rc = ble_gattc_disc_all_dscs(h, start, end, on_dsc, (void *)(intptr_t)id);
    if (rc) conn_fail(id, "discovery failed");
}

static int on_chr(uint16_t handle, const struct ble_gatt_error *error, const struct ble_gatt_chr *chr, void *arg)
{
    int id = (int)(intptr_t)arg;
    int st = error ? error->status : 0;
    if (st == 0 && chr) {
        pthread_mutex_lock(&s_mu);
        int slot = slot_of(id);
        if (slot >= 0 && s_conn[slot].nchr < MAX_CHRS) {
            conn_t *c = &s_conn[slot];
            int i = c->nchr++;
            c->chr[i].svc = c->disc_svc;
            c->chr[i].uuid = chr->uuid;
            c->chr[i].def = chr->def_handle;
            c->chr[i].val = chr->val_handle;
            c->chr[i].props = chr->properties;
            c->chr[i].cccd = 0;
        }
        pthread_mutex_unlock(&s_mu);
    } else if (st == BLE_HS_EDONE) {
        pthread_mutex_lock(&s_mu);
        int slot = slot_of(id);
        if (slot >= 0) s_conn[slot].disc_svc++;
        pthread_mutex_unlock(&s_mu);
        disc_next_chrs(id);
    } else {
        conn_fail(id, "discovery failed");
    }
    return 0;
}

/* Characteristics of each service in turn, then the descriptors. */
static void disc_next_chrs(int id)
{
    uint16_t h = 0, start = 0, end = 0;
    bool dscs = false;
    pthread_mutex_lock(&s_mu);
    int slot = slot_of(id);
    if (slot >= 0 && s_conn[slot].state == C_DISCOVERING) {
        conn_t *c = &s_conn[slot];
        if (c->disc_svc < c->nsvc) {
            h = c->handle;
            start = c->svc[c->disc_svc].start;
            end = c->svc[c->disc_svc].end;
        } else {
            c->disc_chr = 0;
            dscs = true;
        }
    }
    pthread_mutex_unlock(&s_mu);
    if (dscs) disc_next_dscs(id);
    else if (h && ble_gattc_disc_all_chrs(h, start, end, on_chr, (void *)(intptr_t)id)) conn_fail(id, "discovery failed");
}

static int on_svc(uint16_t handle, const struct ble_gatt_error *error, const struct ble_gatt_svc *svc, void *arg)
{
    int id = (int)(intptr_t)arg;
    int st = error ? error->status : 0;
    if (st == 0 && svc) {
        pthread_mutex_lock(&s_mu);
        int slot = slot_of(id);
        if (slot >= 0 && s_conn[slot].nsvc < MAX_SVCS) {
            conn_t *c = &s_conn[slot];
            c->svc[c->nsvc].uuid = svc->uuid;
            c->svc[c->nsvc].start = svc->start_handle;
            c->svc[c->nsvc].end = svc->end_handle;
            c->nsvc++;
        }
        pthread_mutex_unlock(&s_mu);
    } else if (st == BLE_HS_EDONE) {
        disc_next_chrs(id);
    } else {
        conn_fail(id, "discovery failed");
    }
    return 0;
}

static int on_mtu(uint16_t handle, const struct ble_gatt_error *error, uint16_t mtu, void *arg)
{
    int id = (int)(intptr_t)arg;
    if (ble_gattc_disc_all_svcs(handle, on_svc, arg)) conn_fail(id, "discovery failed");
    return 0;
}

/* -------------------------------------------------------- GAP events */

static void notify_store(int id, uint16_t attr, struct os_mbuf *om)
{
    pthread_mutex_lock(&s_mu);
    int slot = slot_of(id);
    if (slot >= 0) {
        conn_t *c = &s_conn[slot];
        for (int i = 0; i < c->nchr; i++) {
            if (c->chr[i].val != attr) continue;
            if (c->note_count == NOTE_N) {           /* full: drop the oldest */
                c->note_head = (c->note_head + 1) % NOTE_N;
                c->note_count--;
            }
            int k = (c->note_head + c->note_count++) % NOTE_N;
            uint16_t n = 0;
            ble_hs_mbuf_to_flat(om, c->note[k].data, NOTE_MAX, &n);
            c->note[k].len = n;
            c->note[k].chr = i;
            break;
        }
    }
    pthread_mutex_unlock(&s_mu);
}

static int conn_event(struct ble_gap_event *ev, void *arg)
{
    int id = (int)(intptr_t)arg;
    switch (ev->type) {
    case BLE_GAP_EVENT_CONNECT: {
        bool ok = ev->connect.status == 0, discover = false;
        uint16_t stale = 0;
        pthread_mutex_lock(&s_mu);
        s_connect_busy = false;
        int slot = slot_of(id);
        if (slot >= 0 && s_conn[slot].state == C_CONNECTING) {
            conn_t *c = &s_conn[slot];
            if (ok) {
                c->handle = ev->connect.conn_handle;
                c->state = C_DISCOVERING;
                discover = true;
            } else {
                c->state = C_CLOSED;
                char m[48];
                err_text(ev->connect.status, m, sizeof m);
                snprintf(c->err, sizeof c->err, "cannot connect: %s", m);
            }
        } else if (ok) {
            stale = ev->connect.conn_handle;          /* closed meanwhile */
        }
        char app[VFS_APP_NAME_MAX] = "";
        if (discover) snprintf(app, sizeof app, "%s", s_conn[slot].app);
        pthread_mutex_unlock(&s_mu);
        if (stale) ble_gap_terminate(stale, BLE_ERR_REM_USER_CONN_TERM);
        /* Bonded with another app: that app's encrypted link isn't ours. */
        struct ble_gap_conn_desc desc;
        char owner[VFS_APP_NAME_MAX];
        if (discover && ble_gap_conn_find(ev->connect.conn_handle, &desc) == 0 &&
            owner_get(&desc.peer_id_addr, owner, sizeof owner) && owner[0] && strcmp(owner, app)) {
            if (!is_bonded(&desc.peer_id_addr)) {
                owner_forget(&desc.peer_id_addr);      /* the bond went away by itself */
            } else {
                pthread_mutex_lock(&s_mu);
                slot = slot_of(id);
                if (slot >= 0 && s_conn[slot].state == C_DISCOVERING) {
                    s_conn[slot].state = C_CLOSED;
                    snprintf(s_conn[slot].err, sizeof s_conn[slot].err, "cannot connect: paired with app '%s'", owner);
                }
                pthread_mutex_unlock(&s_mu);
                ble_gap_terminate(ev->connect.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
                return 0;
            }
        }
        if (discover && ble_gattc_exchange_mtu(ev->connect.conn_handle, on_mtu, arg)) on_mtu(ev->connect.conn_handle, NULL, 23, arg);
        return 0;
    }
    case BLE_GAP_EVENT_DISCONNECT: {
        pthread_mutex_lock(&s_mu);
        int slot = slot_of(id);
        if (slot >= 0 && s_conn[slot].state != C_CLOSED) {
            conn_t *c = &s_conn[slot];
            c->state = C_CLOSED;
            c->handle = 0;
            disconnect_text(ev->disconnect.reason, c->err, sizeof c->err);
            ops_fail_conn(slot, c->err);
        } else if (slot >= 0) {
            s_conn[slot].handle = 0;
        }
        pthread_mutex_unlock(&s_mu);
        return 0;
    }
    case BLE_GAP_EVENT_ENC_CHANGE: {
        int retry[MAX_OPS], nretry = 0;
        char app[VFS_APP_NAME_MAX] = "";
        pthread_mutex_lock(&s_mu);
        int slot = slot_of(id);
        if (slot >= 0) snprintf(app, sizeof app, "%s", s_conn[slot].app);
        pthread_mutex_unlock(&s_mu);
        /* Checked again now that the link is encrypted: only now is the
           peer's identity address known (a private address that couldn't be
           resolved at CONNECT passes the check there). */
        struct ble_gap_conn_desc desc;
        char owner[VFS_APP_NAME_MAX] = "";
        bool found = slot >= 0 && ev->enc_change.status == 0 &&
                     ble_gap_conn_find(ev->enc_change.conn_handle, &desc) == 0;
        bool foreign = found && app[0] && owner_get(&desc.peer_id_addr, owner, sizeof owner) && owner[0] &&
                       strcmp(owner, app);
        pthread_mutex_lock(&s_mu);
        slot = slot_of(id);
        if (slot >= 0) {
            s_conn[slot].securing = false;
            for (int i = 0; i < MAX_OPS; i++) {
                op_t *op = &s_ops[i];
                if (op->state != O_WAIT_SEC || op->slot != slot) continue;
                if (ev->enc_change.status == 0 && !foreign) { op->state = O_PENDING; retry[nretry++] = op->token; }
                else op_fail(op, foreign ? "paired with another app" : "pairing failed");
            }
            if (foreign && s_conn[slot].state != C_CLOSED) {
                s_conn[slot].state = C_CLOSED;
                snprintf(s_conn[slot].err, sizeof s_conn[slot].err, "paired with app '%s'", owner);
                ops_fail_conn(slot, s_conn[slot].err);
            }
        }
        pthread_mutex_unlock(&s_mu);
        if (foreign) {
            ESP_LOGW(TAG, "%s's link is to a device paired with %s: closing it", app, owner);
            ble_gap_terminate(ev->enc_change.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            return 0;
        }
        if (found && app[0] && desc.sec_state.bonded && !owner[0]) owner_set(&desc.peer_id_addr, app);
        for (int i = 0; i < nretry; i++) op_issue(retry[i]);
        return 0;
    }
    case BLE_GAP_EVENT_NOTIFY_RX:
        notify_store(id, ev->notify_rx.attr_handle, ev->notify_rx.om);
        return 0;
    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        /* The peer lost our bond and wants a new one: forget the old one. */
        struct ble_gap_conn_desc desc;
        if (ble_gap_conn_find(ev->repeat_pairing.conn_handle, &desc) == 0) {
            ble_store_util_delete_peer(&desc.peer_id_addr);
            owner_forget(&desc.peer_id_addr);          /* the new bond goes to this connection's app */
        }
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }
    default:
        return 0;
    }
}

/* --------------------------------------------------------- connections */

int ble_connect(const ble_addr_t *addr, const char *app, char *err, size_t errlen)
{
    if (!start(err, errlen)) return -1;
    pthread_mutex_lock(&s_mu);
    int slot = -1;
    for (int i = 0; i < BLE_MAX_CONNS; i++) {
        conn_t *c = &s_conn[i];
        if (c->state != C_FREE && c->state != C_CLOSED && !memcmp(c->addr.val, addr->val, 6)) {
            char a[18];
            ble_addr_format(addr, a);
            snprintf(err, errlen, "already connected to %s", a);
            pthread_mutex_unlock(&s_mu);
            return -1;
        }
    }
    for (int i = 0; i < BLE_MAX_CONNS && slot < 0; i++) if (s_conn[i].state == C_FREE) slot = i;
    for (int i = 0; i < BLE_MAX_CONNS && slot < 0; i++) if (s_conn[i].state == C_CLOSED) slot = i;   /* lost ones */
    if (slot < 0) {
        snprintf(err, errlen, "too many Bluetooth connections (at most %d)", BLE_MAX_CONNS);
        pthread_mutex_unlock(&s_mu);
        return -1;
    }
    conn_t *c = &s_conn[slot];
    int gen = c->gen + 1;
    memset(c, 0, sizeof *c);
    c->gen = gen & 0xfffff;
    c->state = C_QUEUED;
    c->addr = *addr;
    snprintf(c->app, sizeof c->app, "%s", app ? app : "");
    /* The address type as advertised, if we heard the device. */
    for (int i = 0; i < MAX_ADV; i++)
        if (s_adv[i].used && !memcmp(s_adv[i].addr.val, addr->val, 6)) c->addr.type = s_adv[i].addr.type;
    int id = conn_id(slot);
    pthread_mutex_unlock(&s_mu);
    ble_poll();
    return id;
}

ble_state_t ble_state(int id, char *err, size_t errlen)
{
    ble_state_t st;
    pthread_mutex_lock(&s_mu);
    int slot = slot_of(id);
    if (slot < 0) { snprintf(err, errlen, "closed"); st = BLE_CLOSED; }
    else if (s_conn[slot].state == C_READY) st = BLE_CONNECTED;
    else if (s_conn[slot].state == C_CLOSED) { snprintf(err, errlen, "%s", s_conn[slot].err); st = BLE_CLOSED; }
    else st = BLE_CONNECTING;
    pthread_mutex_unlock(&s_mu);
    return st;
}

/* Under s_mu: free the slot; returns the link to take down, if any (0 none,
   -1 a connection attempt to cancel). */
static int release_locked(int slot)
{
    conn_t *c = &s_conn[slot];
    int r = 0;
    if (c->state == C_CONNECTING) r = -1;
    else if (c->state != C_FREE && c->handle) r = c->handle;
    ops_fail_conn(slot, "closed");
    c->state = C_FREE;
    c->handle = 0;
    c->gen = (c->gen + 1) & 0xfffff;
    return r;
}

static void take_down(int r)
{
    if (r > 0) ble_gap_terminate((uint16_t)r, BLE_ERR_REM_USER_CONN_TERM);
    else if (r < 0) ble_gap_conn_cancel();
}

void ble_close(int id)
{
    pthread_mutex_lock(&s_mu);
    int slot = slot_of(id), r = 0;
    if (slot >= 0) r = release_locked(slot);
    pthread_mutex_unlock(&s_mu);
    take_down(r);
}

int ble_chr_count(int id)
{
    pthread_mutex_lock(&s_mu);
    int slot = slot_of(id);
    int n = slot >= 0 && s_conn[slot].state == C_READY ? s_conn[slot].nchr : 0;
    pthread_mutex_unlock(&s_mu);
    return n;
}

bool ble_chr_info(int id, int chr, ble_chr_info_t *out)
{
    pthread_mutex_lock(&s_mu);
    int slot = slot_of(id);
    bool ok = slot >= 0 && s_conn[slot].state == C_READY && chr >= 0 && chr < s_conn[slot].nchr;
    if (ok) {
        conn_t *c = &s_conn[slot];
        out->svc = c->svc[c->chr[chr].svc].uuid;
        out->uuid = c->chr[chr].uuid;
        out->props = c->chr[chr].props;
    }
    pthread_mutex_unlock(&s_mu);
    return ok;
}

int ble_chr_find(int id, const ble_uuid_any_t *svc, const ble_uuid_any_t *uuid)
{
    int found = -1;
    pthread_mutex_lock(&s_mu);
    int slot = slot_of(id);
    if (slot >= 0 && s_conn[slot].state == C_READY) {
        conn_t *c = &s_conn[slot];
        for (int i = 0; i < c->nchr && found < 0; i++)
            if (!ble_uuid_cmp(&c->chr[i].uuid.u, &uuid->u) && !ble_uuid_cmp(&c->svc[c->chr[i].svc].uuid.u, &svc->u))
                found = i;
    }
    pthread_mutex_unlock(&s_mu);
    return found;
}

bool ble_notified(int id, int *chr, uint8_t *buf, size_t cap, size_t *len)
{
    bool got = false;
    pthread_mutex_lock(&s_mu);
    int slot = slot_of(id);
    if (slot >= 0 && s_conn[slot].note_count) {
        conn_t *c = &s_conn[slot];
        int k = c->note_head;
        size_t n = c->note[k].len < cap ? c->note[k].len : cap;
        memcpy(buf, c->note[k].data, n);
        *len = n;
        *chr = c->note[k].chr;
        c->note_head = (k + 1) % NOTE_N;
        c->note_count--;
        got = true;
    }
    pthread_mutex_unlock(&s_mu);
    return got;
}

/* ------------------------------------------------------------- the loop */

void ble_poll(void)
{
    if (!s_started || !s_synced) return;

    /* A connection attempt, if one is queued and none is running. */
    ble_addr_t addr;
    int connect_id = -1;
    bool cancel_scan = false;
    pthread_mutex_lock(&s_mu);
    uint32_t now = hw_millis();
    for (int i = 0; i < MAX_OPS; i++)
        if ((s_ops[i].state == O_PENDING || s_ops[i].state == O_WAIT_SEC) && (int32_t)(now - s_ops[i].deadline) > 0)
            op_fail(&s_ops[i], "timeout");
    if (!s_connect_busy) {
        for (int i = 0; i < BLE_MAX_CONNS; i++) {
            if (s_conn[i].state != C_QUEUED) continue;
            s_conn[i].state = C_CONNECTING;
            s_connect_busy = true;
            addr = s_conn[i].addr;
            connect_id = conn_id(i);
            if (s_scanning) { s_scanning = false; cancel_scan = true; }
            break;
        }
    }
    pthread_mutex_unlock(&s_mu);
    if (cancel_scan) ble_gap_disc_cancel();
    if (connect_id >= 0) {
        int rc = ble_gap_connect(s_own_addr_type, &addr, CONNECT_TIMEOUT_MS, NULL, conn_event, (void *)(intptr_t)connect_id);
        if (rc) {
            pthread_mutex_lock(&s_mu);
            s_connect_busy = false;
            int slot = slot_of(connect_id);
            if (slot >= 0) {
                char m[48];
                err_text(rc, m, sizeof m);
                s_conn[slot].state = C_CLOSED;
                snprintf(s_conn[slot].err, sizeof s_conn[slot].err, "cannot connect: %s", m);
            }
            pthread_mutex_unlock(&s_mu);
        }
    }

    /* Scanning: on while wanted, paused while connecting. */
    pthread_mutex_lock(&s_mu);
    bool want = s_want_scan && !s_connect_busy, on = s_scanning, active = s_scan_active;
    if (want != on) s_scanning = want;
    pthread_mutex_unlock(&s_mu);
    if (want && !on) {
        struct ble_gap_disc_params p = { .itvl = SCAN_ITVL, .window = SCAN_WINDOW, .passive = !active,
                                         .filter_duplicates = 0 };
        int rc = ble_gap_disc(s_own_addr_type, BLE_HS_FOREVER, &p, scan_event, NULL);
        if (rc && rc != BLE_HS_EALREADY) {
            ESP_LOGW(TAG, "scan didn't start: %d", rc);
            pthread_mutex_lock(&s_mu);
            s_scanning = false;
            pthread_mutex_unlock(&s_mu);
        }
    } else if (!want && on) {
        ble_gap_disc_cancel();
    }
}

void ble_reset(void)
{
    if (!s_started) return;
    int down[BLE_MAX_CONNS];
    pthread_mutex_lock(&s_mu);
    s_want_scan = false;
    for (int i = 0; i < BLE_MAX_CONNS; i++) down[i] = release_locked(i);
    for (int i = 0; i < MAX_OPS; i++) s_ops[i].state = O_FREE;
    memset(s_adv, 0, sizeof s_adv);
    pthread_mutex_unlock(&s_mu);
    for (int i = 0; i < BLE_MAX_CONNS; i++) take_down(down[i]);
    ble_poll();
}

bool ble_busy(void)
{
    if (!s_started) return false;
    pthread_mutex_lock(&s_mu);
    bool busy = s_want_scan || s_scanning || s_connect_busy;
    for (int i = 0; !busy && s_conn && i < BLE_MAX_CONNS; i++)
        busy = s_conn[i].state != C_FREE && s_conn[i].state != C_CLOSED;
    pthread_mutex_unlock(&s_mu);
    return busy;
}

/* ---------------------------------------------------- UUIDs, addresses */

/* The Bluetooth base UUID 0000xxxx-0000-1000-8000-00805f9b34fb, little-endian. */
static const uint8_t BASE_UUID[16] = { 0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80,
                                       0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };

bool ble_uuid_parse(const char *s, ble_uuid_any_t *out)
{
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    uint8_t be[16];
    int n = 0, hi = -1;
    for (; *s; s++) {
        if (*s == '-') continue;
        int v = hex_digit(*s);
        if (v < 0 || n >= 16) return false;
        if (hi < 0) hi = v;
        else { be[n++] = (uint8_t)(hi << 4 | v); hi = -1; }
    }
    if (hi >= 0) return false;
    if (n == 2) {
        out->u16.u.type = BLE_UUID_TYPE_16;
        out->u16.value = (uint16_t)(be[0] << 8 | be[1]);
        return true;
    }
    if (n == 4) {
        out->u32.u.type = BLE_UUID_TYPE_32;
        out->u32.value = (uint32_t)be[0] << 24 | (uint32_t)be[1] << 16 | (uint32_t)be[2] << 8 | be[3];
        return true;
    }
    if (n != 16) return false;
    uint8_t le[16];
    for (int i = 0; i < 16; i++) le[i] = be[15 - i];
    if (!memcmp(le, BASE_UUID, 12)) {                     /* a SIG-assigned one, written long */
        uint32_t v = (uint32_t)le[12] | (uint32_t)le[13] << 8 | (uint32_t)le[14] << 16 | (uint32_t)le[15] << 24;
        if (v <= 0xffff) { out->u16.u.type = BLE_UUID_TYPE_16; out->u16.value = (uint16_t)v; }
        else { out->u32.u.type = BLE_UUID_TYPE_32; out->u32.value = v; }
        return true;
    }
    out->u128.u.type = BLE_UUID_TYPE_128;
    memcpy(out->u128.value, le, 16);
    return true;
}

void ble_uuid_format(const ble_uuid_any_t *u, char out[BLE_UUID_STR_LEN])
{
    if (u->u.type == BLE_UUID_TYPE_16) { snprintf(out, BLE_UUID_STR_LEN, "%04x", u->u16.value); return; }
    if (u->u.type == BLE_UUID_TYPE_32) { snprintf(out, BLE_UUID_STR_LEN, "%08x", (unsigned)u->u32.value); return; }
    const uint8_t *v = u->u128.value;
    char *p = out;
    for (int i = 15; i >= 0; i--) {
        p += sprintf(p, "%02x", v[i]);
        if (i == 12 || i == 10 || i == 8 || i == 6) *p++ = '-';
    }
    *p = 0;
}

bool ble_addr_parse(const char *s, ble_addr_t *out)
{
    uint8_t w[6];
    for (int i = 0; i < 6; i++) {
        int a = hex_digit(s[0]), b = a < 0 ? -1 : hex_digit(s[1]);
        if (b < 0) return false;
        w[i] = (uint8_t)(a << 4 | b);
        s += 2;
        if (i < 5) { if (*s != ':' && *s != '-') return false; s++; }
    }
    if (*s) return false;
    for (int i = 0; i < 6; i++) out->val[i] = w[5 - i];
    /* Until we hear it advertise: the top bits 11 mark a random static address. */
    out->type = (w[0] & 0xc0) == 0xc0 ? BLE_ADDR_RANDOM : BLE_ADDR_PUBLIC;
    return true;
}

void ble_addr_format(const ble_addr_t *a, char out[18])
{
    snprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x",
             a->val[5], a->val[4], a->val[3], a->val[2], a->val[1], a->val[0]);
}
