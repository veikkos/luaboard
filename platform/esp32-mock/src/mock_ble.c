/*
 * ESP32 mock platform - NimBLE host (GAP central, GATT client).
 *
 * Plays the part of the NimBLE host stack for the firmware: the same calls,
 * the same events, delivered on the host task (a thread here) like on the
 * chip. The peers are either simulated devices (sim_ble_add, the simulator's
 * device models) or real ones nearby through the computer's own Bluetooth
 * (mock_ble_host.m), which the firmware can't tell apart.
 *
 * NimBLE identifies a peer's services, characteristics and descriptors by
 * attribute handles. The simulated devices and CoreBluetooth know them only
 * by UUID, so each connection gets a made-up attribute table when it comes
 * up: service, then per characteristic its declaration, value and (if it
 * can notify or indicate) its CCCD descriptor, numbered from 1.
 *
 * Like the real stack, callbacks never run inside the call that asked for
 * them: everything is queued and run on the host task, in order, with delays
 * on the chip's clock (so the simulator's fast-forward speeds them up too).
 */
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "host/ble_hs.h"
#include "mock_ble_internal.h"
#include "mock_internal.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "sim_hw.h"

static const char *TAG = "mock_ble";

struct ble_hs_cfg ble_hs_cfg;

#define MAX_DEVS    8
#define MAX_LINKS   3         /* CONFIG_BT_NIMBLE_MAX_CONNECTIONS */
#define MAX_OPS     16        /* GATT requests waiting for the host backend */
#define MAX_SEEN    64        /* host devices seen while scanning (to connect by address) */
#define ADV_EVERY_MS 200      /* a simulated device advertises this often */
#define CONNECT_MS   60       /* simulated connection set-up */
#define SPVN_MS      1000     /* supervision timeout: a peer out of range is lost after this */
#define MTU          247
#define MAX_BONDS    8        /* CONFIG_BT_NIMBLE_MAX_BONDS */

/* ---------------------------------------------------------------- work */

/* The host task's queue: functions to run at (or after) a time on the
   chip's clock, in order of time, then of posting. */
typedef struct work {
    int64_t due_us;
    uint64_t seq;
    void (*fn)(struct work *w);
    int link, gen, chr, status;
    uint16_t handle;
    void *cb, *arg;
    size_t len;
    struct work *next;
    uint8_t data[];
} work_t;

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_cond = PTHREAD_COND_INITIALIZER;
static work_t *s_queue;
static uint64_t s_seq;
static bool s_running, s_stop, s_inited;
static pthread_t s_task;

static int64_t now_us(void) { return esp_timer_get_time(); }

static work_t *work_new(void (*fn)(work_t *), size_t len)
{
    work_t *w = calloc(1, sizeof *w + len);
    if (!w) abort();
    w->fn = fn;
    w->len = len;
    w->link = -1;
    return w;
}

/* Queue w to run after delay_ms. Called with s_lock held. */
static void post_locked(work_t *w, uint32_t delay_ms)
{
    w->due_us = now_us() + (int64_t)delay_ms * 1000;
    w->seq = s_seq++;
    work_t **p = &s_queue;
    while (*p && ((*p)->due_us < w->due_us || ((*p)->due_us == w->due_us && (*p)->seq < w->seq)))
        p = &(*p)->next;
    w->next = *p;
    *p = w;
    sim_cond_signal(&s_cond);
}

/* ---------------------------------------------------------- devices, links */

typedef struct {
    const sim_ble_device_t *d;
    bool present;
    bool reported;            /* advertised once in this scan (duplicate filter) */
    int rssi;
} fdev_t;

typedef enum { L_FREE, L_CONNECTING, L_UP } lstate_t;

typedef enum { OP_READ, OP_WRITE, OP_SUB } opkind_t;
typedef struct {
    bool used;
    opkind_t kind;
    int chr;
    uint16_t handle;
    uint16_t cccd;            /* OP_SUB: the value being written */
    ble_gatt_attr_fn *cb;
    void *arg;
} op_t;

typedef struct {
    lstate_t state;
    int gen;                  /* bumped whenever the link changes hands: stale work is dropped */
    int dev;                  /* simulated device, or -1: through the host backend */
    ble_addr_t addr;
    ble_gap_event_fn *cb;
    void *arg;
    bool encrypted;
    int nchrs;
    mock_ble_chr_t chrs[MOCK_BLE_MAX_CHRS];
    uint16_t def_h[MOCK_BLE_MAX_CHRS], cccd_h[MOCK_BLE_MAX_CHRS];   /* value handle = def_h + 1 */
    uint16_t sub[MOCK_BLE_MAX_CHRS];                                 /* CCCD value */
    op_t ops[MAX_OPS];
} link_t;

/* Bonds made by pairing with simulated devices, oldest first. (Kept in RAM:
   a restarted simulator has none. A real device through the host backend
   pairs with the computer, which keeps its own.) */
static ble_addr_t s_bond[MAX_BONDS];
static int s_nbond;

static int bond_index(const ble_addr_t *a)
{
    for (int i = 0; i < s_nbond; i++) if (!memcmp(s_bond[i].val, a->val, 6)) return i;
    return -1;
}

static fdev_t s_dev[MAX_DEVS];
static int s_ndev;
static link_t s_link[MAX_LINKS];

static struct {
    bool on;
    int gen;
    bool passive, filter_dups;
    ble_gap_event_fn *cb;
    void *arg;
} s_scan;

static bool s_host_wanted;    /* sim_ble_use_host() */
static bool s_host;           /* the host backend is in use */
static struct { uint8_t addr[6]; } s_seen[MAX_SEEN];
static int s_nseen, s_seen_next;

static uint16_t conn_handle(int link) { return (uint16_t)(link + 1); }

static int link_of(uint16_t handle)
{
    int i = (int)handle - 1;
    return i >= 0 && i < MAX_LINKS && s_link[i].state == L_UP ? i : -1;
}

/* ble_addr_t holds the address little-endian; the simulator API as written. */
static void addr_from_written(ble_addr_t *a, const uint8_t w[6], bool random)
{
    a->type = random ? BLE_ADDR_RANDOM : BLE_ADDR_PUBLIC;
    for (int i = 0; i < 6; i++) a->val[i] = w[5 - i];
}

static void addr_to_written(const ble_addr_t *a, uint8_t w[6])
{
    for (int i = 0; i < 6; i++) w[i] = a->val[5 - i];
}

static int find_dev(const ble_addr_t *a)
{
    uint8_t w[6];
    addr_to_written(a, w);
    for (int i = 0; i < s_ndev; i++) if (!memcmp(s_dev[i].d->addr, w, 6)) return i;
    return -1;
}

static bool host_seen(const uint8_t w[6])
{
    for (int i = 0; i < s_nseen; i++) if (!memcmp(s_seen[i].addr, w, 6)) return true;
    return false;
}

/* Number the attributes of a link that just came up. */
static void assign_handles(link_t *l)
{
    uint16_t h = 1;
    for (int i = 0; i < l->nchrs; i++) {
        if (i == 0 || ble_uuid_cmp(&l->chrs[i].svc.u, &l->chrs[i - 1].svc.u)) h++;   /* service */
        l->def_h[i] = h;
        h += 2;
        l->cccd_h[i] = (l->chrs[i].props & (BLE_GATT_CHR_PROP_NOTIFY | BLE_GATT_CHR_PROP_INDICATE)) ? h++ : 0;
        l->sub[i] = 0;
    }
}

/* The service around characteristic i: [first chr index, last chr index]. */
static void svc_span(const link_t *l, int i, int *first, int *last)
{
    int a = i, b = i;
    while (a > 0 && !ble_uuid_cmp(&l->chrs[a - 1].svc.u, &l->chrs[i].svc.u)) a--;
    while (b + 1 < l->nchrs && !ble_uuid_cmp(&l->chrs[b + 1].svc.u, &l->chrs[i].svc.u)) b++;
    *first = a;
    *last = b;
}

static uint16_t chr_end(const link_t *l, int i)
{
    return l->cccd_h[i] ? l->cccd_h[i] : (uint16_t)(l->def_h[i] + 1);
}

static work_t *link_work(void (*fn)(work_t *), int link, size_t len)
{
    work_t *w = work_new(fn, len);
    w->link = link;
    w->gen = s_link[link].gen;
    return w;
}

/* Under s_lock: is this work still for the same connection? */
static bool work_live(const work_t *w, lstate_t want)
{
    return w->link >= 0 && s_link[w->link].gen == w->gen && s_link[w->link].state == want;
}

/* ------------------------------------------------------------- GAP events */

static void w_gap_event(work_t *w);

/* A GAP event for the link's callback. ev->type and fields are set by the
   caller; the conn handle is filled in where the event has one. */
static void post_gap(int link, const struct ble_gap_event *ev, uint32_t delay_ms)
{
    work_t *w = link_work(w_gap_event, link, sizeof *ev);
    memcpy(w->data, ev, sizeof *ev);
    post_locked(w, delay_ms);
}

static void w_gap_event(work_t *w)
{
    struct ble_gap_event ev;
    memcpy(&ev, w->data, sizeof ev);
    pthread_mutex_lock(&s_lock);
    link_t *l = &s_link[w->link];
    bool live = l->gen == w->gen && l->state != L_FREE;
    ble_gap_event_fn *cb = l->cb;
    void *arg = l->arg;
    if (live && ev.type == BLE_GAP_EVENT_ENC_CHANGE && ev.enc_change.status == 0) l->encrypted = true;
    pthread_mutex_unlock(&s_lock);
    if (live && cb) cb(&ev, arg);
}

/* Take the link down: pending GATT requests fail first (BLE_HS_ENOTCONN, as
   in NimBLE), then BLE_GAP_EVENT_DISCONNECT. Under s_lock; the callbacks run
   later on the host task. */
static void w_op_fail(work_t *w);
static void w_disconnected(work_t *w);

static void link_down_locked(int link, int reason, uint32_t delay_ms)
{
    link_t *l = &s_link[link];
    if (l->state == L_FREE) return;
    for (int i = 0; i < MAX_OPS; i++) {
        if (!l->ops[i].used) continue;
        work_t *w = work_new(w_op_fail, 0);
        w->link = link;
        w->handle = l->ops[i].handle;
        w->cb = (void *)l->ops[i].cb;
        w->arg = l->ops[i].arg;
        w->status = BLE_HS_ENOTCONN;
        post_locked(w, delay_ms);
        l->ops[i].used = false;
    }
    work_t *w = link_work(w_disconnected, link, 0);
    w->status = reason;
    post_locked(w, delay_ms);
}

static void w_op_fail(work_t *w)
{
    struct ble_gatt_error err = { .status = (uint16_t)w->status, .att_handle = w->handle };
    ((ble_gatt_attr_fn *)w->cb)(conn_handle(w->link), &err, NULL, w->arg);
}

static void w_disconnected(work_t *w)
{
    pthread_mutex_lock(&s_lock);
    link_t *l = &s_link[w->link];
    if (l->gen != w->gen || l->state == L_FREE) { pthread_mutex_unlock(&s_lock); return; }
    bool was_up = l->state == L_UP;
    struct ble_gap_event ev = { 0 };
    if (was_up) {
        ev.type = BLE_GAP_EVENT_DISCONNECT;
        ev.disconnect.reason = w->status;
        ev.disconnect.conn.conn_handle = conn_handle(w->link);
        ev.disconnect.conn.peer_id_addr = l->addr;
        ev.disconnect.conn.peer_ota_addr = l->addr;
    } else {                                    /* the connection never came up */
        ev.type = BLE_GAP_EVENT_CONNECT;
        ev.connect.status = w->status;
        ev.connect.conn_handle = conn_handle(w->link);
    }
    ble_gap_event_fn *cb = l->cb;
    void *arg = l->arg;
    l->state = L_FREE;
    l->gen++;
    pthread_mutex_unlock(&s_lock);
    if (cb) cb(&ev, arg);
}

/* A connection came up with these characteristics. Under s_lock. */
static void link_up_locked(int link, const mock_ble_chr_t *chrs, int n)
{
    link_t *l = &s_link[link];
    l->nchrs = n < MOCK_BLE_MAX_CHRS ? n : MOCK_BLE_MAX_CHRS;
    memcpy(l->chrs, chrs, sizeof *chrs * (size_t)l->nchrs);
    assign_handles(l);
    l->state = L_UP;
    l->encrypted = false;
    struct ble_gap_event ev = { .type = BLE_GAP_EVENT_CONNECT };
    ev.connect.status = 0;
    ev.connect.conn_handle = conn_handle(link);
    post_gap(link, &ev, 0);
}

/* ------------------------------------------------------------------ scan */

static void w_scan_event(work_t *w)
{
    pthread_mutex_lock(&s_lock);
    bool live = s_scan.on && s_scan.gen == w->gen;
    ble_gap_event_fn *cb = s_scan.cb;
    void *arg = s_scan.arg;
    pthread_mutex_unlock(&s_lock);
    if (!live || !cb) return;
    struct ble_gap_event ev = { .type = BLE_GAP_EVENT_DISC };
    memcpy(&ev.disc, w->data, sizeof ev.disc);
    ev.disc.data = w->data + sizeof ev.disc;
    cb(&ev, arg);
}

static void post_report_locked(uint8_t type, const ble_addr_t *a, int rssi, const uint8_t *ad, size_t len)
{
    work_t *w = work_new(w_scan_event, sizeof(struct ble_gap_disc_desc) + len);
    w->gen = s_scan.gen;
    struct ble_gap_disc_desc d = { .event_type = type, .length_data = (uint8_t)len, .addr = *a, .rssi = (int8_t)rssi };
    memcpy(w->data, &d, sizeof d);
    memcpy(w->data + sizeof d, ad, len);
    post_locked(w, 0);
}

/* An advertisement holds 31 bytes; what doesn't fit (CoreBluetooth hands
   over everything it heard at once) goes in a scan response, whole AD
   structures each, as a device would send it. */
static void post_adv_locked(const ble_addr_t *a, int rssi, const uint8_t *ad, size_t len)
{
    uint8_t part[2][31] = { { 0 } };
    size_t plen[2] = { 0, 0 };
    for (size_t i = 0; i < len && ad[i];) {
        size_t n = 1 + (size_t)ad[i];
        if (i + n > len) break;
        int k = plen[0] + n <= 31 ? 0 : plen[1] + n <= 31 ? 1 : -1;
        if (k >= 0) { memcpy(part[k] + plen[k], ad + i, n); plen[k] += n; }
        i += n;
    }
    post_report_locked(BLE_HCI_ADV_RPT_EVTYPE_ADV_IND, a, rssi, part[0], plen[0]);
    if (plen[1]) post_report_locked(BLE_HCI_ADV_RPT_EVTYPE_SCAN_RSP, a, rssi, part[1], plen[1]);
}

/* Every ADV_EVERY_MS while scanning: the simulated devices in range advertise. */
static void w_adv_tick(work_t *w)
{
    pthread_mutex_lock(&s_lock);
    if (s_scan.on && s_scan.gen == w->gen) {
        for (int i = 0; i < s_ndev; i++) {
            fdev_t *f = &s_dev[i];
            if (!f->present || (s_scan.filter_dups && f->reported)) continue;
            f->reported = true;
            ble_addr_t a;
            addr_from_written(&a, f->d->addr, f->d->random);
            post_adv_locked(&a, f->rssi, f->d->adv, f->d->adv_len);
        }
        work_t *next = work_new(w_adv_tick, 0);
        next->gen = w->gen;
        post_locked(next, ADV_EVERY_MS);
    }
    pthread_mutex_unlock(&s_lock);
}

static void w_scan_done(work_t *w)
{
    pthread_mutex_lock(&s_lock);
    bool live = s_scan.on && s_scan.gen == w->gen;
    ble_gap_event_fn *cb = s_scan.cb;
    void *arg = s_scan.arg;
    if (live) {
        s_scan.on = false;
        s_scan.gen++;
        if (s_host) mock_ble_host_scan(false, false, false);
    }
    pthread_mutex_unlock(&s_lock);
    if (!live || !cb) return;
    struct ble_gap_event ev = { .type = BLE_GAP_EVENT_DISC_COMPLETE };
    ev.disc_complete.reason = 0;
    cb(&ev, arg);
}

static bool connecting_locked(void)
{
    for (int i = 0; i < MAX_LINKS; i++) if (s_link[i].state == L_CONNECTING) return true;
    return false;
}

int ble_gap_disc(uint8_t own_addr_type, int32_t duration_ms, const struct ble_gap_disc_params *p,
                 ble_gap_event_fn *cb, void *cb_arg)
{
    pthread_mutex_lock(&s_lock);
    int rc = 0;
    if (!s_running) rc = BLE_HS_ENOTSYNCED;
    else if (s_scan.on) rc = BLE_HS_EALREADY;
    else if (connecting_locked()) rc = BLE_HS_EBUSY;
    if (rc) { pthread_mutex_unlock(&s_lock); return rc; }
    s_scan.on = true;
    s_scan.gen++;
    s_scan.passive = p ? p->passive : false;
    s_scan.filter_dups = p ? p->filter_duplicates : false;
    s_scan.cb = cb;
    s_scan.arg = cb_arg;
    for (int i = 0; i < s_ndev; i++) s_dev[i].reported = false;
    work_t *w = work_new(w_adv_tick, 0);
    w->gen = s_scan.gen;
    post_locked(w, 10);
    if (duration_ms != BLE_HS_FOREVER) {
        work_t *d = work_new(w_scan_done, 0);
        d->gen = s_scan.gen;
        post_locked(d, (uint32_t)(duration_ms > 0 ? duration_ms : 0));
    }
    if (s_host) mock_ble_host_scan(true, !s_scan.passive, !s_scan.filter_dups);
    pthread_mutex_unlock(&s_lock);
    return 0;
}

int ble_gap_disc_cancel(void)
{
    pthread_mutex_lock(&s_lock);
    int rc = s_scan.on ? 0 : BLE_HS_EALREADY;
    if (s_scan.on) {
        s_scan.on = false;
        s_scan.gen++;
        if (s_host) mock_ble_host_scan(false, false, false);
    }
    pthread_mutex_unlock(&s_lock);
    return rc;
}

int ble_gap_disc_active(void)
{
    pthread_mutex_lock(&s_lock);
    int on = s_scan.on;
    pthread_mutex_unlock(&s_lock);
    return on;
}

/* --------------------------------------------------------------- connect */

static void w_fake_connected(work_t *w)
{
    pthread_mutex_lock(&s_lock);
    if (work_live(w, L_CONNECTING)) {
        link_t *l = &s_link[w->link];
        fdev_t *f = &s_dev[l->dev];
        if (f->present) {
            mock_ble_chr_t chrs[MOCK_BLE_MAX_CHRS];
            int n = f->d->nchrs < MOCK_BLE_MAX_CHRS ? f->d->nchrs : MOCK_BLE_MAX_CHRS;
            for (int i = 0; i < n; i++) {
                chrs[i].svc = f->d->chrs[i].svc;
                chrs[i].uuid = f->d->chrs[i].uuid;
                chrs[i].props = f->d->chrs[i].props;
            }
            link_up_locked(w->link, chrs, n);
        }
        /* not in range: stays connecting until the timeout */
    }
    pthread_mutex_unlock(&s_lock);
}

static void w_connect_timeout(work_t *w)
{
    pthread_mutex_lock(&s_lock);
    if (work_live(w, L_CONNECTING)) {
        if (s_link[w->link].dev < 0) mock_ble_host_disconnect(w->link);
        link_down_locked(w->link, w->status, 0);
    }
    pthread_mutex_unlock(&s_lock);
}

int ble_gap_connect(uint8_t own_addr_type, const ble_addr_t *peer_addr, int32_t duration_ms,
                    const struct ble_gap_conn_params *params, ble_gap_event_fn *cb, void *cb_arg)
{
    pthread_mutex_lock(&s_lock);
    int rc = 0, link = -1;
    if (!s_running) rc = BLE_HS_ENOTSYNCED;
    else if (s_scan.on) rc = BLE_HS_EBUSY;           /* (no BLE_HOST_ALLOW_CONNECT_WITH_SCAN) */
    else if (connecting_locked()) rc = BLE_HS_EALREADY;
    else {
        for (int i = 0; i < MAX_LINKS; i++)
            if (s_link[i].state == L_UP && !memcmp(s_link[i].addr.val, peer_addr->val, 6)) rc = BLE_HS_EDONE;
        for (int i = 0; i < MAX_LINKS && link < 0 && !rc; i++) if (s_link[i].state == L_FREE) link = i;
        if (!rc && link < 0) rc = BLE_HS_ENOMEM;
    }
    if (rc) { pthread_mutex_unlock(&s_lock); return rc; }

    link_t *l = &s_link[link];
    l->gen++;
    l->state = L_CONNECTING;
    l->addr = *peer_addr;
    l->cb = cb;
    l->arg = cb_arg;
    l->nchrs = 0;
    memset(l->ops, 0, sizeof l->ops);
    l->dev = find_dev(peer_addr);
    if (l->dev >= 0) {
        post_locked(link_work(w_fake_connected, link, 0), CONNECT_MS);
    } else {
        uint8_t w[6];
        addr_to_written(peer_addr, w);
        if (s_host && host_seen(w)) mock_ble_host_connect(link, w);
        /* else nobody answers: the timeout below ends it */
    }
    if (duration_ms <= 0 || duration_ms == BLE_HS_FOREVER) duration_ms = 30000;
    work_t *t = link_work(w_connect_timeout, link, 0);
    t->status = BLE_HS_ETIMEOUT;
    post_locked(t, (uint32_t)duration_ms);
    pthread_mutex_unlock(&s_lock);
    return 0;
}

int ble_gap_conn_cancel(void)
{
    pthread_mutex_lock(&s_lock);
    int rc = BLE_HS_EALREADY;
    for (int i = 0; i < MAX_LINKS; i++) {
        if (s_link[i].state != L_CONNECTING) continue;
        if (s_link[i].dev < 0) mock_ble_host_disconnect(i);
        link_down_locked(i, BLE_HS_EAPP, 0);
        rc = 0;
    }
    pthread_mutex_unlock(&s_lock);
    return rc;
}

int ble_gap_terminate(uint16_t handle, uint8_t hci_reason)
{
    pthread_mutex_lock(&s_lock);
    int link = link_of(handle);
    if (link < 0) { pthread_mutex_unlock(&s_lock); return BLE_HS_ENOTCONN; }
    if (s_link[link].dev >= 0) link_down_locked(link, BLE_HS_HCI_ERR(BLE_ERR_CONN_TERM_LOCAL), 20);
    else mock_ble_host_disconnect(link);              /* mock_ble_host_disconnected follows */
    pthread_mutex_unlock(&s_lock);
    return 0;
}

int ble_gap_conn_find(uint16_t handle, struct ble_gap_conn_desc *out)
{
    pthread_mutex_lock(&s_lock);
    int link = link_of(handle);
    if (link >= 0 && out) {
        memset(out, 0, sizeof *out);
        out->conn_handle = handle;
        out->peer_id_addr = out->peer_ota_addr = s_link[link].addr;
        out->sec_state.encrypted = s_link[link].encrypted;
        out->sec_state.bonded = s_link[link].encrypted && bond_index(&s_link[link].addr) >= 0;
    }
    pthread_mutex_unlock(&s_lock);
    return link < 0 ? BLE_HS_ENOTCONN : 0;
}

/* Pairing "just works": the link is encrypted a moment later. (Through the
   host backend the computer pairs by itself when a device asks for it.) */
int ble_gap_security_initiate(uint16_t handle)
{
    pthread_mutex_lock(&s_lock);
    int link = link_of(handle);
    ble_addr_t peer = link >= 0 ? s_link[link].addr : (ble_addr_t){ 0 };
    bool sim = link >= 0 && s_link[link].dev >= 0, full = s_nbond == MAX_BONDS && bond_index(&peer) < 0;
    pthread_mutex_unlock(&s_lock);
    if (link < 0) return BLE_HS_ENOTCONN;
    /* Storing a new bond with the store full: like NimBLE, ask the
       application (store_status_cb) to make room. */
    int status = 0;
    if (sim && full && ble_hs_cfg.store_status_cb) {
        union ble_store_value v = { .sec = { .peer_addr = peer } };
        struct ble_store_status_event e = { .event_code = BLE_STORE_EVENT_OVERFLOW };
        e.overflow.obj_type = BLE_STORE_OBJ_TYPE_OUR_SEC;
        e.overflow.value = &v;
        if (ble_hs_cfg.store_status_cb(&e, ble_hs_cfg.store_status_arg) != 0) status = BLE_HS_ESTORE_CAP;
    }
    pthread_mutex_lock(&s_lock);
    link = link_of(handle);
    if (link >= 0) {
        if (sim && !status && bond_index(&peer) < 0) {
            if (s_nbond == MAX_BONDS) status = BLE_HS_ESTORE_CAP;
            else s_bond[s_nbond++] = peer;
        }
        struct ble_gap_event ev = { .type = BLE_GAP_EVENT_ENC_CHANGE };
        ev.enc_change.status = status;
        ev.enc_change.conn_handle = handle;
        post_gap(link, &ev, 100);
    }
    pthread_mutex_unlock(&s_lock);
    return link < 0 ? BLE_HS_ENOTCONN : 0;
}

int ble_gap_unpair(const ble_addr_t *peer)
{
    pthread_mutex_lock(&s_lock);
    int i = bond_index(peer);
    if (i >= 0) {
        memmove(&s_bond[i], &s_bond[i + 1], (size_t)(s_nbond - i - 1) * sizeof s_bond[0]);
        s_nbond--;
    }
    for (int l = 0; l < MAX_LINKS; l++) {
        if (s_link[l].state != L_UP || memcmp(s_link[l].addr.val, peer->val, 6)) continue;
        if (s_link[l].dev >= 0) link_down_locked(l, BLE_HS_HCI_ERR(BLE_ERR_CONN_TERM_LOCAL), 20);
        else mock_ble_host_disconnect(l);
    }
    pthread_mutex_unlock(&s_lock);
    return 0;
}

int ble_store_util_bonded_peers(ble_addr_t *out, int *out_num, int max)
{
    pthread_mutex_lock(&s_lock);
    int n = s_nbond < max ? s_nbond : max;
    memcpy(out, s_bond, (size_t)n * sizeof *out);
    *out_num = n;
    pthread_mutex_unlock(&s_lock);
    return 0;
}

/* ------------------------------------------------------------ discovery */

static void w_mtu(work_t *w)
{
    struct ble_gatt_error err = { .status = 0 };
    ((ble_gatt_mtu_fn *)w->cb)(conn_handle(w->link), &err, MTU, w->arg);
}

int ble_gattc_exchange_mtu(uint16_t handle, ble_gatt_mtu_fn *cb, void *cb_arg)
{
    pthread_mutex_lock(&s_lock);
    int link = link_of(handle);
    if (link >= 0) {
        work_t *w = link_work(w_mtu, link, 0);
        w->cb = (void *)cb;
        w->arg = cb_arg;
        post_locked(w, 5);
    }
    pthread_mutex_unlock(&s_lock);
    return link < 0 ? BLE_HS_ENOTCONN : 0;
}

/* Discovery answers come from a snapshot of the table taken when asked,
   one callback per item and a last one with BLE_HS_EDONE, like NimBLE. */
typedef struct { int kind; uint16_t start, end; } disc_req_t;

static void w_disc(work_t *w)
{
    disc_req_t q;
    memcpy(&q, w->data, sizeof q);
    pthread_mutex_lock(&s_lock);
    bool live = work_live(w, L_UP);
    link_t snap;
    if (live) snap = s_link[w->link];
    pthread_mutex_unlock(&s_lock);
    uint16_t ch = conn_handle(w->link);
    struct ble_gatt_error done = { .status = live ? BLE_HS_EDONE : BLE_HS_ENOTCONN };
    if (live) {
        for (int i = 0; i < snap.nchrs; i++) {
            int first, last;
            svc_span(&snap, i, &first, &last);
            if (q.kind == 0 && i == first) {
                struct ble_gatt_svc s = { .start_handle = (uint16_t)(snap.def_h[first] - 1),
                                          .end_handle = chr_end(&snap, last), .uuid = snap.chrs[i].svc };
                struct ble_gatt_error ok = { 0 };
                if (((ble_gatt_disc_svc_fn *)w->cb)(ch, &ok, &s, w->arg)) return;
            } else if (q.kind == 1 && snap.def_h[i] >= q.start && snap.def_h[i] <= q.end) {
                struct ble_gatt_chr c = { .def_handle = snap.def_h[i], .val_handle = (uint16_t)(snap.def_h[i] + 1),
                                          .properties = snap.chrs[i].props, .uuid = snap.chrs[i].uuid };
                struct ble_gatt_error ok = { 0 };
                if (((ble_gatt_chr_fn *)w->cb)(ch, &ok, &c, w->arg)) return;
            } else if (q.kind == 2 && snap.cccd_h[i] && snap.cccd_h[i] > q.start && snap.cccd_h[i] <= q.end) {
                struct ble_gatt_dsc d = { .handle = snap.cccd_h[i] };
                d.uuid.u16.u.type = BLE_UUID_TYPE_16;
                d.uuid.u16.value = BLE_GATT_DSC_CLT_CFG_UUID16;
                struct ble_gatt_error ok = { 0 };
                if (((ble_gatt_dsc_fn *)w->cb)(ch, &ok, (uint16_t)(snap.def_h[i] + 1), &d, w->arg)) return;
            }
        }
    }
    switch (q.kind) {
    case 0: ((ble_gatt_disc_svc_fn *)w->cb)(ch, &done, NULL, w->arg); break;
    case 1: ((ble_gatt_chr_fn *)w->cb)(ch, &done, NULL, w->arg); break;
    default: ((ble_gatt_dsc_fn *)w->cb)(ch, &done, 0, NULL, w->arg); break;
    }
}

static int post_disc(uint16_t handle, int kind, uint16_t start, uint16_t end, void *cb, void *arg)
{
    pthread_mutex_lock(&s_lock);
    int link = link_of(handle);
    if (link >= 0) {
        work_t *w = link_work(w_disc, link, sizeof(disc_req_t));
        disc_req_t q = { kind, start, end };
        memcpy(w->data, &q, sizeof q);
        w->cb = cb;
        w->arg = arg;
        post_locked(w, 5);
    }
    pthread_mutex_unlock(&s_lock);
    return link < 0 ? BLE_HS_ENOTCONN : 0;
}

int ble_gattc_disc_all_svcs(uint16_t handle, ble_gatt_disc_svc_fn *cb, void *cb_arg)
{
    return post_disc(handle, 0, 1, 0xffff, (void *)cb, cb_arg);
}

int ble_gattc_disc_all_chrs(uint16_t handle, uint16_t start, uint16_t end, ble_gatt_chr_fn *cb, void *cb_arg)
{
    return post_disc(handle, 1, start, end, (void *)cb, cb_arg);
}

int ble_gattc_disc_all_dscs(uint16_t handle, uint16_t start, uint16_t end, ble_gatt_dsc_fn *cb, void *cb_arg)
{
    return post_disc(handle, 2, start, end, (void *)cb, cb_arg);
}

/* ------------------------------------------------------- read and write */

/* Answer a GATT request: status 0 with data (reads) or an error. */
static void w_attr_done(work_t *w)
{
    pthread_mutex_lock(&s_lock);
    bool live = work_live(w, L_UP);
    pthread_mutex_unlock(&s_lock);
    if (!live) return;                       /* the disconnect already failed it */
    struct ble_gatt_error err = { .status = (uint16_t)w->status, .att_handle = w->handle };
    static struct os_mbuf om;                /* only the host task uses it */
    struct ble_gatt_attr attr = { .handle = w->handle, .offset = 0, .om = &om };
    om.hdr.omp_len = (uint16_t)(w->len < sizeof om.data ? w->len : sizeof om.data);
    memcpy(om.data, w->data, om.hdr.omp_len);
    ((ble_gatt_attr_fn *)w->cb)(conn_handle(w->link), &err, w->status ? NULL : &attr, w->arg);
}

static void post_attr_locked(int link, uint16_t handle, int status, const void *data, size_t len,
                             ble_gatt_attr_fn *cb, void *arg, uint32_t delay_ms)
{
    if (!cb) return;
    work_t *w = link_work(w_attr_done, link, len);
    w->handle = handle;
    w->status = status;
    w->cb = (void *)cb;
    w->arg = arg;
    if (len) memcpy(w->data, data, len);
    post_locked(w, delay_ms);
}

/* A simulated device's read or write handler runs on the host task too. */
static void w_fake_rw(work_t *w)
{
    pthread_mutex_lock(&s_lock);
    bool live = work_live(w, L_UP);
    const sim_ble_device_t *d = live ? s_dev[s_link[w->link].dev].d : NULL;
    pthread_mutex_unlock(&s_lock);
    if (!live) return;
    uint8_t buf[512];
    int status = 0;
    size_t len = 0;
    if (w->status == OP_READ) {
        int n = d->on_read ? d->on_read(d->ctx, w->chr, buf, sizeof buf) : -BLE_ATT_ERR_READ_NOT_PERMITTED;
        if (n < 0) status = BLE_HS_ATT_ERR(-n);
        else len = (size_t)n;
    } else {
        int e = d->on_write ? d->on_write(d->ctx, w->chr, w->data, w->len) : BLE_ATT_ERR_WRITE_NOT_PERMITTED;
        status = BLE_HS_ATT_ERR(e);
    }
    if (!w->cb) return;                      /* write without response */
    pthread_mutex_lock(&s_lock);
    post_attr_locked(w->link, w->handle, status, buf, len, (ble_gatt_attr_fn *)w->cb, w->arg, 0);
    pthread_mutex_unlock(&s_lock);
}

/* Tell a simulated device about a subscription (after the write's answer,
   so a value it sends at once arrives subscribed). */
static void w_fake_sub(work_t *w)
{
    pthread_mutex_lock(&s_lock);
    bool live = work_live(w, L_UP);
    const sim_ble_device_t *d = live ? s_dev[s_link[w->link].dev].d : NULL;
    pthread_mutex_unlock(&s_lock);
    if (live) d->on_subscribe(d->ctx, w->chr, w->status != 0);
}

/* Which characteristic a handle belongs to: its value (cccd = false) or its
   CCCD (cccd = true). -1 if neither. */
static int chr_of(const link_t *l, uint16_t h, bool *cccd)
{
    for (int i = 0; i < l->nchrs; i++) {
        if (h == l->def_h[i] + 1) { *cccd = false; return i; }
        if (l->cccd_h[i] && h == l->cccd_h[i]) { *cccd = true; return i; }
    }
    return -1;
}

static int add_op(link_t *l, opkind_t kind, int chr, uint16_t h, uint16_t cccd, ble_gatt_attr_fn *cb, void *arg)
{
    for (int i = 0; i < MAX_OPS; i++) {
        if (l->ops[i].used) continue;
        l->ops[i] = (op_t){ .used = true, .kind = kind, .chr = chr, .handle = h, .cccd = cccd, .cb = cb, .arg = arg };
        return 0;
    }
    return BLE_HS_ENOMEM;
}

/* The checks every access goes through: properties, then pairing. */
static int access_check(const link_t *l, int chr, uint8_t need)
{
    if (!(l->chrs[chr].props & need))
        return BLE_HS_ERR_ATT_BASE + (need == BLE_GATT_CHR_PROP_READ ? BLE_ATT_ERR_READ_NOT_PERMITTED
                                                                      : BLE_ATT_ERR_WRITE_NOT_PERMITTED);
    if (l->dev >= 0 && (s_dev[l->dev].d->chrs[chr].flags & SIM_BLE_ENCRYPTED) && !l->encrypted)
        return BLE_HS_ATT_ERR(BLE_ATT_ERR_INSUFFICIENT_AUTHEN);
    return 0;
}

int ble_gattc_read(uint16_t handle, uint16_t attr, ble_gatt_attr_fn *cb, void *cb_arg)
{
    pthread_mutex_lock(&s_lock);
    int link = link_of(handle), rc = 0;
    if (link < 0) { pthread_mutex_unlock(&s_lock); return BLE_HS_ENOTCONN; }
    link_t *l = &s_link[link];
    bool cccd;
    int chr = chr_of(l, attr, &cccd);
    if (chr < 0) {
        post_attr_locked(link, attr, BLE_HS_ATT_ERR(BLE_ATT_ERR_INVALID_HANDLE), NULL, 0, cb, cb_arg, 5);
    } else if (cccd) {
        uint8_t v[2] = { (uint8_t)l->sub[chr], (uint8_t)(l->sub[chr] >> 8) };
        post_attr_locked(link, attr, 0, v, 2, cb, cb_arg, 5);
    } else if ((rc = access_check(l, chr, BLE_GATT_CHR_PROP_READ))) {
        post_attr_locked(link, attr, rc, NULL, 0, cb, cb_arg, 5);
        rc = 0;
    } else if (l->dev >= 0) {
        work_t *w = link_work(w_fake_rw, link, 0);
        w->status = OP_READ;
        w->chr = chr;
        w->handle = attr;
        w->cb = (void *)cb;
        w->arg = cb_arg;
        post_locked(w, 10);
    } else if (!(rc = add_op(l, OP_READ, chr, attr, 0, cb, cb_arg))) {
        mock_ble_host_read(link, chr);
    }
    pthread_mutex_unlock(&s_lock);
    return rc;
}

static int write_common(uint16_t handle, uint16_t attr, const void *data, uint16_t len,
                        ble_gatt_attr_fn *cb, void *cb_arg, bool response)
{
    pthread_mutex_lock(&s_lock);
    int link = link_of(handle), rc = 0;
    if (link < 0) { pthread_mutex_unlock(&s_lock); return BLE_HS_ENOTCONN; }
    link_t *l = &s_link[link];
    bool cccd;
    int chr = chr_of(l, attr, &cccd);
    if (chr < 0) {
        if (response) post_attr_locked(link, attr, BLE_HS_ATT_ERR(BLE_ATT_ERR_INVALID_HANDLE), NULL, 0, cb, cb_arg, 5);
    } else if (cccd) {
        const uint8_t *d = data;
        uint16_t v = len >= 2 ? (uint16_t)(d[0] | d[1] << 8) : len == 1 ? d[0] : 0;
        bool locked = l->dev >= 0 && (s_dev[l->dev].d->chrs[chr].flags & SIM_BLE_ENCRYPTED) && !l->encrypted;
        if (locked) {
            if (response) post_attr_locked(link, attr, BLE_HS_ATT_ERR(BLE_ATT_ERR_INSUFFICIENT_AUTHEN), NULL, 0, cb, cb_arg, 5);
        } else if (l->dev >= 0) {
            l->sub[chr] = v;
            if (response) post_attr_locked(link, attr, 0, NULL, 0, cb, cb_arg, 10);
            if (s_dev[l->dev].d->on_subscribe) {
                work_t *w = link_work(w_fake_sub, link, 0);
                w->chr = chr;
                w->status = v != 0;
                post_locked(w, 15);
            }
        } else if (!(rc = add_op(l, OP_SUB, chr, attr, v, response ? cb : NULL, cb_arg))) {
            mock_ble_host_subscribe(link, chr, v != 0);
        }
    } else if ((rc = access_check(l, chr, response ? BLE_GATT_CHR_PROP_WRITE : BLE_GATT_CHR_PROP_WRITE_NO_RSP))) {
        if (response) post_attr_locked(link, attr, rc, NULL, 0, cb, cb_arg, 5);
        rc = 0;
    } else if (l->dev >= 0) {
        work_t *w = link_work(w_fake_rw, link, len);
        w->status = OP_WRITE;
        w->chr = chr;
        w->handle = attr;
        w->cb = response ? (void *)cb : NULL;
        w->arg = cb_arg;
        memcpy(w->data, data, len);
        post_locked(w, 10);
    } else if (!response || !(rc = add_op(l, OP_WRITE, chr, attr, 0, cb, cb_arg))) {
        mock_ble_host_write(link, chr, data, len, response);
    }
    pthread_mutex_unlock(&s_lock);
    return rc;
}

int ble_gattc_write_flat(uint16_t handle, uint16_t attr, const void *data, uint16_t len,
                         ble_gatt_attr_fn *cb, void *cb_arg)
{
    return write_common(handle, attr, data, len, cb, cb_arg, true);
}

int ble_gattc_write_no_rsp_flat(uint16_t handle, uint16_t attr, const void *data, uint16_t len)
{
    return write_common(handle, attr, data, len, NULL, NULL, false);
}

/* ----------------------------------------------------------- notifications */

static void w_notify(work_t *w);

static void post_notify_locked(int link, int chr, const uint8_t *data, size_t len)
{
    link_t *l = &s_link[link];
    if (l->state != L_UP || chr < 0 || chr >= l->nchrs || !l->sub[chr]) return;
    struct ble_gap_event ev = { .type = BLE_GAP_EVENT_NOTIFY_RX };
    ev.notify_rx.attr_handle = (uint16_t)(l->def_h[chr] + 1);
    ev.notify_rx.conn_handle = conn_handle(link);
    ev.notify_rx.indication = (l->sub[chr] & 2) && !(l->sub[chr] & 1);
    /* the mbuf travels after the event: w_notify fixes up the pointer */
    work_t *w = link_work(w_notify, link, sizeof ev + sizeof(struct os_mbuf));
    struct os_mbuf *om = (struct os_mbuf *)(w->data + sizeof ev);
    om->hdr.omp_len = (uint16_t)(len < sizeof om->data ? len : sizeof om->data);
    memcpy(om->data, data, om->hdr.omp_len);
    memcpy(w->data, &ev, sizeof ev);
    post_locked(w, 0);
}

static void w_notify(work_t *w)
{
    struct ble_gap_event ev;
    memcpy(&ev, w->data, sizeof ev);
    ev.notify_rx.om = (struct os_mbuf *)(w->data + sizeof ev);
    pthread_mutex_lock(&s_lock);
    bool live = work_live(w, L_UP);
    ble_gap_event_fn *cb = s_link[w->link].cb;
    void *arg = s_link[w->link].arg;
    pthread_mutex_unlock(&s_lock);
    if (live && cb) cb(&ev, arg);
}

/* ------------------------------------------------------ simulator side */

int sim_ble_add(const sim_ble_device_t *dev)
{
    pthread_mutex_lock(&s_lock);
    int id = -1;
    if (s_ndev < MAX_DEVS && dev->nchrs <= MOCK_BLE_MAX_CHRS) {
        fdev_t *f = &s_dev[s_ndev];
        memset(f, 0, sizeof *f);
        f->d = dev;
        f->rssi = -60;
        id = s_ndev++;
    }
    pthread_mutex_unlock(&s_lock);
    return id;
}

void sim_ble_set_present(int id, bool present)
{
    pthread_mutex_lock(&s_lock);
    if (id >= 0 && id < s_ndev && s_dev[id].present != present) {
        s_dev[id].present = present;
        s_dev[id].reported = false;
        if (!present)
            for (int i = 0; i < MAX_LINKS; i++)
                if (s_link[i].state == L_UP && s_link[i].dev == id)
                    link_down_locked(i, BLE_HS_HCI_ERR(BLE_ERR_CONN_SPVN_TMO), SPVN_MS);
    }
    pthread_mutex_unlock(&s_lock);
}

void sim_ble_set_rssi(int id, int rssi)
{
    pthread_mutex_lock(&s_lock);
    if (id >= 0 && id < s_ndev) s_dev[id].rssi = rssi;
    pthread_mutex_unlock(&s_lock);
}

void sim_ble_notify(int id, int chr, const uint8_t *data, size_t len)
{
    pthread_mutex_lock(&s_lock);
    for (int i = 0; i < MAX_LINKS; i++)
        if (s_link[i].state == L_UP && s_link[i].dev == id) post_notify_locked(i, chr, data, len);
    pthread_mutex_unlock(&s_lock);
}

void sim_ble_use_host(bool on)
{
    pthread_mutex_lock(&s_lock);
    s_host_wanted = on;
    pthread_mutex_unlock(&s_lock);
}

/* ------------------------------------------------------ host backend side */

void mock_ble_host_adv(const uint8_t addr[6], int rssi, const uint8_t *ad, size_t len)
{
    pthread_mutex_lock(&s_lock);
    if (!host_seen(addr)) {
        memcpy(s_seen[s_seen_next].addr, addr, 6);
        s_seen_next = (s_seen_next + 1) % MAX_SEEN;
        if (s_nseen < MAX_SEEN) s_nseen++;
    }
    if (s_scan.on) {
        ble_addr_t a;
        addr_from_written(&a, addr, true);   /* CoreBluetooth hides the real one: made up, random */
        post_adv_locked(&a, rssi, ad, len);
    }
    pthread_mutex_unlock(&s_lock);
}

void mock_ble_host_connected(int link, const mock_ble_chr_t *chrs, int n)
{
    pthread_mutex_lock(&s_lock);
    if (link >= 0 && link < MAX_LINKS && s_link[link].state == L_CONNECTING && s_link[link].dev < 0)
        link_up_locked(link, chrs, n);
    pthread_mutex_unlock(&s_lock);
}

void mock_ble_host_disconnected(int link, int hci_reason)
{
    pthread_mutex_lock(&s_lock);
    if (link >= 0 && link < MAX_LINKS && s_link[link].dev < 0)
        link_down_locked(link, BLE_HS_HCI_ERR(hci_reason), 0);
    pthread_mutex_unlock(&s_lock);
}

/* The oldest request of that kind for that characteristic: answers come
   back in the order they were asked. */
static op_t *take_op(link_t *l, opkind_t kind, int chr)
{
    for (int i = 0; i < MAX_OPS; i++)
        if (l->ops[i].used && l->ops[i].kind == kind && l->ops[i].chr == chr) {
            l->ops[i].used = false;
            return &l->ops[i];
        }
    return NULL;
}

static void host_done(int link, opkind_t kind, int chr, int att_err, const uint8_t *data, size_t len)
{
    pthread_mutex_lock(&s_lock);
    if (link >= 0 && link < MAX_LINKS && s_link[link].state == L_UP) {
        link_t *l = &s_link[link];
        op_t *op = take_op(l, kind, chr);
        if (op) {
            if (kind == OP_SUB && !att_err) l->sub[chr] = op->cccd;
            if (att_err == BLE_ATT_ERR_INSUFFICIENT_AUTHEN || att_err == BLE_ATT_ERR_INSUFFICIENT_ENC)
                ESP_LOGW(TAG, "the device wants pairing: allow it in the computer's Bluetooth dialog");
            post_attr_locked(link, op->handle, BLE_HS_ATT_ERR(att_err), data, len, op->cb, op->arg, 0);
        }
    }
    pthread_mutex_unlock(&s_lock);
}

void mock_ble_host_read_done(int link, int chr, int att_err, const uint8_t *data, size_t len)
{
    host_done(link, OP_READ, chr, att_err, data, len);
}

void mock_ble_host_write_done(int link, int chr, int att_err) { host_done(link, OP_WRITE, chr, att_err, NULL, 0); }

void mock_ble_host_subscribe_done(int link, int chr, int att_err) { host_done(link, OP_SUB, chr, att_err, NULL, 0); }

void mock_ble_host_value(int link, int chr, const uint8_t *data, size_t len)
{
    pthread_mutex_lock(&s_lock);
    if (link >= 0 && link < MAX_LINKS) post_notify_locked(link, chr, data, len);
    pthread_mutex_unlock(&s_lock);
}

/* ------------------------------------------------------------ the host */

esp_err_t nimble_port_init(void)
{
    pthread_mutex_lock(&s_lock);
    bool host = s_host_wanted && !s_inited;
    s_inited = true;
    pthread_mutex_unlock(&s_lock);
    if (host && mock_ble_host_start()) {
        pthread_mutex_lock(&s_lock);
        s_host = true;
        pthread_mutex_unlock(&s_lock);
    }
    return ESP_OK;
}

static void *task_main(void *fn)
{
    ((TaskFunction_t)fn)(NULL);
    return NULL;
}

void nimble_port_freertos_init(TaskFunction_t host_task_fn)
{
    pthread_create(&s_task, NULL, task_main, (void *)host_task_fn);
    pthread_detach(s_task);
}

void nimble_port_freertos_deinit(void) {}

void nimble_port_run(void)
{
    pthread_mutex_lock(&s_lock);
    s_running = true;
    s_stop = false;
    pthread_mutex_unlock(&s_lock);
    if (ble_hs_cfg.sync_cb) ble_hs_cfg.sync_cb();     /* the controller is up */

    pthread_mutex_lock(&s_lock);
    while (!s_stop) {
        work_t *w = s_queue;
        if (w && w->due_us <= now_us()) {
            s_queue = w->next;
            pthread_mutex_unlock(&s_lock);
            w->fn(w);
            free(w);
            pthread_mutex_lock(&s_lock);
            continue;
        }
        /* Wait for new work or the next due time (it can return early: the
           loop looks again). A fast-forward stops at that time for this. */
        sim_cond_wait_until(&s_cond, &s_lock, w ? w->due_us : INT64_MAX);
    }
    s_running = false;
    pthread_mutex_unlock(&s_lock);
}

int nimble_port_stop(void)
{
    pthread_mutex_lock(&s_lock);
    s_stop = true;
    pthread_cond_signal(&s_cond);
    pthread_mutex_unlock(&s_lock);
    return 0;
}

/* ------------------------------------------------------------ odds and ends */

void ble_store_config_init(void) {}
int ble_store_util_status_rr(struct ble_store_status_event *event, void *arg) { return 0; }
int ble_store_util_delete_peer(const ble_addr_t *peer_id_addr)
{
    pthread_mutex_lock(&s_lock);
    int i = bond_index(peer_id_addr);
    if (i >= 0) {
        memmove(&s_bond[i], &s_bond[i + 1], (size_t)(s_nbond - i - 1) * sizeof s_bond[0]);
        s_nbond--;
    }
    pthread_mutex_unlock(&s_lock);
    return 0;
}

int ble_hs_id_infer_auto(int privacy, uint8_t *out_addr_type)
{
    if (out_addr_type) *out_addr_type = BLE_ADDR_PUBLIC;
    return 0;
}

int ble_hs_mbuf_to_flat(const struct os_mbuf *om, void *flat, uint16_t max_len, uint16_t *out_copy_len)
{
    uint16_t n = om->hdr.omp_len < max_len ? om->hdr.omp_len : max_len;
    memcpy(flat, om->data, n);
    if (out_copy_len) *out_copy_len = n;
    return n < om->hdr.omp_len ? BLE_HS_EINVAL : 0;
}

int ble_uuid_cmp(const ble_uuid_t *a, const ble_uuid_t *b)
{
    if (a->type != b->type) return (int)a->type - (int)b->type;
    switch (a->type) {
    case BLE_UUID_TYPE_16: return (int)((const ble_uuid16_t *)a)->value - (int)((const ble_uuid16_t *)b)->value;
    case BLE_UUID_TYPE_32: {
        uint32_t x = ((const ble_uuid32_t *)a)->value, y = ((const ble_uuid32_t *)b)->value;
        return x < y ? -1 : x > y;
    }
    default: return memcmp(((const ble_uuid128_t *)a)->value, ((const ble_uuid128_t *)b)->value, 16);
    }
}

int ble_uuid_init_from_buf(ble_uuid_any_t *u, const void *buf, size_t len)
{
    const uint8_t *b = buf;
    switch (len) {
    case 2: u->u16.u.type = BLE_UUID_TYPE_16; u->u16.value = (uint16_t)(b[0] | b[1] << 8); return 0;
    case 4: u->u32.u.type = BLE_UUID_TYPE_32; u->u32.value = (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24; return 0;
    case 16: u->u128.u.type = BLE_UUID_TYPE_128; memcpy(u->u128.value, b, 16); return 0;
    default: return BLE_HS_EINVAL;
    }
}

