/*
 * ESP32 mock platform - Wi-Fi station, default event loop, esp_netif,
 * lwIP bind() port mapping and mDNS.
 *
 * The station "associates" ~300 ms after esp_wifi_connect() if the simulated
 * access point is available (sim_wifi_set_ap), then gets IP 127.0.0.1. The
 * firmware's sockets are the host's real sockets, so its TCP server is
 * reachable on this computer. mDNS announcements go through the host's.
 */
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "mdns.h"
#include "mock_internal.h"
#include "sim_hw.h"

#undef bind      /* the real ones, for the mock's own implementation */
#undef connect
#ifndef _WIN32
#undef send
#undef recv
#undef sendto
#undef recvfrom
#undef accept

ssize_t mock_lwip_send(int s, const void *buf, size_t len, int flags)
{
    ssize_t n = send(s, buf, len, flags);
    if (n > 0) sim_net_sent(s);
    return n;
}

ssize_t mock_lwip_recv(int s, void *buf, size_t len, int flags)
{
    ssize_t n = recv(s, buf, len, flags);
    if (n >= 0) sim_net_received(s);                     /* (0: the other end closed it, an answer too) */
    /* More may be there already (the firmware takes so much a pass), or
       come any moment: either way the tick's wait ends when it is. */
    if (n > 0 || (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))) sim_net_waits(s);
    return n;
}

/* A UDP socket: what it sends may be a question, so the clock sees it as
   it does a connection the firmware made. */
ssize_t mock_lwip_sendto(int s, const void *buf, size_t len, int flags, const struct sockaddr *to, socklen_t tolen)
{
    ssize_t n = sendto(s, buf, len, flags, to, tolen);
    if (n >= 0) {
        sim_net_client(s);
        sim_net_sent(s);
    }
    return n;
}

ssize_t mock_lwip_recvfrom(int s, void *buf, size_t len, int flags, struct sockaddr *from, socklen_t *fromlen)
{
    ssize_t n = recvfrom(s, buf, len, flags, from, fromlen);
    if (n >= 0) sim_net_received(s);
    if (n >= 0 || errno == EAGAIN || errno == EWOULDBLOCK) sim_net_waits(s);
    return n;
}

int mock_lwip_accept(int s, struct sockaddr *addr, socklen_t *len)
{
    /* (in a fast-forward not every pass, like the PC link: sim_link_look) */
    if (!sim_link_look()) {
        errno = EAGAIN;
        return -1;
    }
    int c = accept(s, addr, len);
    if (c >= 0) sim_net_forget(c);                       /* (its number may be a closed connection's) */
    else if (errno == EAGAIN || errno == EWOULDBLOCK) sim_net_waits(s);
    return c;
}
#endif
#ifdef _WIN32
#define bind mock_win_bind          /* Winsock's, with errors in errno */
#define connect mock_win_connect
#endif

/* A name under .invalid has no address, by definition (RFC 6761). The
   simulator says so itself, the way a network does where nobody answers the
   question: after LOOKUP_NONE_MS of the chip's clock. Every other name goes
   to the computer's resolver, which takes its own, real time over a name
   nobody knows (5 s on macOS for a .local one). */
#define LOOKUP_NONE_MS 3000
#undef getaddrinfo

int mock_lwip_getaddrinfo(const char *node, const char *service, const struct addrinfo *hints, struct addrinfo **res)
{
    static const char none[] = ".invalid";
    size_t n = node ? strlen(node) : 0, m = sizeof none - 1, i = 0;
    while (n >= m && i < m && (node[n - m + i] | 0x20) == none[i]) i++;
    if (i == m) {
        mock_sleep_ms(LOOKUP_NONE_MS);
        return EAI_NONAME;
    }
    return getaddrinfo(node, service, hints, res);
}

static const char *TAG = "wifi";

esp_event_base_t const WIFI_EVENT = "WIFI_EVENT";
esp_event_base_t const IP_EVENT = "IP_EVENT";

/* ------------------------------------------------------------ event loop */

#define MAX_HANDLERS 8
#define MAX_QUEUE 16

typedef struct { esp_event_base_t base; int32_t id; esp_event_handler_t fn; void *arg; } handler_t;
typedef struct { esp_event_base_t base; int32_t id; uint8_t data[64]; } event_t;

static handler_t s_handlers[MAX_HANDLERS];
static int s_nhandlers;
static event_t s_queue[MAX_QUEUE];
static int s_qhead, s_qlen;
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_cond = PTHREAD_COND_INITIALIZER;
static int s_loop_started;

static void *loop_thread(void *arg)
{
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&s_lock);
        while (s_qlen == 0) sim_cond_wait(&s_cond, &s_lock);
        event_t ev = s_queue[s_qhead];
        s_qhead = (s_qhead + 1) % MAX_QUEUE;
        s_qlen--;
        handler_t hs[MAX_HANDLERS];
        int n = s_nhandlers;
        memcpy(hs, s_handlers, sizeof hs);
        pthread_mutex_unlock(&s_lock);
        for (int i = 0; i < n; i++)
            if (hs[i].base == ev.base && (hs[i].id == ESP_EVENT_ANY_ID || hs[i].id == ev.id))
                hs[i].fn(hs[i].arg, ev.base, ev.id, ev.data);
    }
    return NULL;
}

static void post(esp_event_base_t base, int32_t id, const void *data, size_t len)
{
    pthread_mutex_lock(&s_lock);
    if (s_qlen < MAX_QUEUE) {
        event_t *ev = &s_queue[(s_qhead + s_qlen) % MAX_QUEUE];
        ev->base = base;
        ev->id = id;
        memset(ev->data, 0, sizeof ev->data);
        if (data && len) memcpy(ev->data, data, len < sizeof ev->data ? len : sizeof ev->data);
        s_qlen++;
        sim_cond_signal(&s_cond);
    }
    pthread_mutex_unlock(&s_lock);
}

esp_err_t esp_event_loop_create_default(void)
{
    pthread_mutex_lock(&s_lock);
    if (s_loop_started) { pthread_mutex_unlock(&s_lock); return ESP_ERR_INVALID_STATE; }
    s_loop_started = 1;
    pthread_mutex_unlock(&s_lock);
    pthread_t t;
    pthread_create(&t, NULL, loop_thread, NULL);
    pthread_detach(t);
    return ESP_OK;
}

esp_err_t esp_event_handler_instance_register(esp_event_base_t base, int32_t id,
                                              esp_event_handler_t handler, void *arg,
                                              esp_event_handler_instance_t *instance)
{
    pthread_mutex_lock(&s_lock);
    if (s_nhandlers >= MAX_HANDLERS) { pthread_mutex_unlock(&s_lock); return ESP_ERR_NO_MEM; }
    s_handlers[s_nhandlers++] = (handler_t){ base, id, handler, arg };
    pthread_mutex_unlock(&s_lock);
    if (instance) *instance = (void *)(intptr_t)s_nhandlers;
    return ESP_OK;
}

/* ----------------------------------------------------------------- netif */

struct esp_netif_obj { char hostname[33]; esp_netif_ip_info_t ip; };
static struct esp_netif_obj s_sta;

esp_err_t esp_netif_init(void) { return ESP_OK; }
esp_netif_t *esp_netif_create_default_wifi_sta(void) { return &s_sta; }

esp_err_t esp_netif_set_hostname(esp_netif_t *n, const char *h)
{
    if (!n || !h) return ESP_ERR_INVALID_ARG;
    snprintf(n->hostname, sizeof n->hostname, "%s", h);
    return ESP_OK;
}

esp_err_t esp_netif_get_ip_info(esp_netif_t *n, esp_netif_ip_info_t *ip)
{
    if (!n || !ip) return ESP_ERR_INVALID_ARG;
    pthread_mutex_lock(&s_lock);
    *ip = n->ip;
    pthread_mutex_unlock(&s_lock);
    return ESP_OK;
}

/* ------------------------------------------------------------ Wi-Fi STA */

static int s_inited, s_started, s_ap_available = 1, s_connected;
static int s_rssi = -52;                 /* the access point's signal, dBm */
static unsigned s_gen;     /* invalidates pending association attempts */
static wifi_config_t s_cfg;

static char s_net[33];                   /* --set wifi_net: "" none */

void sim_wifi_set_net(const char *ssid)
{
    snprintf(s_net, sizeof s_net, "%s", ssid && strcmp(ssid, "none") ? ssid : "");
}

const char *sim_wifi_net(void) { return s_net[0] ? s_net : NULL; }

void sim_wifi_set_ap(int available)
{
    pthread_mutex_lock(&s_lock);
    s_ap_available = available;
    int drop = !available && s_connected;
    if (drop) { s_connected = 0; s_gen++; memset(&s_sta.ip, 0, sizeof s_sta.ip); }
    pthread_mutex_unlock(&s_lock);
    if (drop) {
        wifi_event_sta_disconnected_t d = { .reason = 200 /* BEACON_TIMEOUT */, .rssi = -90 };
        post(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &d, sizeof d);
    }
}

int sim_wifi_connected(void)
{
    pthread_mutex_lock(&s_lock);
    int c = s_connected;
    pthread_mutex_unlock(&s_lock);
    return c;
}

esp_err_t esp_wifi_init(const wifi_init_config_t *config)
{
    if (!config || config->magic != 0x1F2F3F4F) return ESP_ERR_INVALID_ARG;
    s_inited = 1;
    return ESP_OK;
}

/* The mock never stores the Wi-Fi config anywhere: RAM is all there is. */
esp_err_t esp_wifi_set_storage(wifi_storage_t storage)
{
    if (!s_inited) return ESP_ERR_INVALID_STATE;
    return storage == WIFI_STORAGE_RAM || storage == WIFI_STORAGE_FLASH ? ESP_OK : ESP_ERR_INVALID_ARG;
}

esp_err_t esp_wifi_set_mode(wifi_mode_t mode)
{
    if (!s_inited) return ESP_ERR_INVALID_STATE;
    return mode == WIFI_MODE_STA ? ESP_OK : ESP_ERR_NOT_SUPPORTED;
}

esp_err_t esp_wifi_set_config(wifi_interface_t iface, wifi_config_t *conf)
{
    if (!s_inited) return ESP_ERR_INVALID_STATE;
    if (iface != WIFI_IF_STA || !conf) return ESP_ERR_INVALID_ARG;
    pthread_mutex_lock(&s_lock);
    s_cfg = *conf;
    pthread_mutex_unlock(&s_lock);
    return ESP_OK;
}

esp_err_t esp_wifi_start(void)
{
    if (!s_inited) return ESP_ERR_INVALID_STATE;
    s_started = 1;
    post(WIFI_EVENT, WIFI_EVENT_STA_START, NULL, 0);
    return ESP_OK;
}

esp_err_t esp_wifi_stop(void)
{
    esp_wifi_disconnect();                               /* an association under way is stale now too */
    pthread_mutex_lock(&s_lock);
    s_started = 0;
    pthread_mutex_unlock(&s_lock);
    post(WIFI_EVENT, WIFI_EVENT_STA_STOP, NULL, 0);
    return ESP_OK;
}

static void *assoc_thread(void *arg)
{
    unsigned gen = (unsigned)(uintptr_t)arg;
    mock_sleep_ms(300);                                  /* association takes a moment */
    pthread_mutex_lock(&s_lock);
    int stale = gen != s_gen || !s_started;
    int ok = !stale && s_ap_available && s_cfg.sta.ssid[0];
    if (ok) {
        s_connected = 1;
        s_sta.ip.ip.addr = htonl(INADDR_LOOPBACK);          /* 127.0.0.1 */
        s_sta.ip.netmask.addr = htonl(0xFF000000);
    }
    esp_netif_ip_info_t ip = s_sta.ip;
    wifi_config_t cfg = s_cfg;
    pthread_mutex_unlock(&s_lock);
    if (stale) { sim_time_idle(); return NULL; }
    if (ok) {
        post(WIFI_EVENT, WIFI_EVENT_STA_CONNECTED, NULL, 0);
        ip_event_got_ip_t got = { .esp_netif = &s_sta, .ip_info = ip, .ip_changed = true };
        post(IP_EVENT, IP_EVENT_STA_GOT_IP, &got, sizeof got);
        ESP_LOGI(TAG, "associated with \"%s\" (simulated AP)", (const char *)cfg.sta.ssid);
    } else {
        wifi_event_sta_disconnected_t d = { .reason = 201 /* NO_AP_FOUND */ };
        post(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &d, sizeof d);
    }
    sim_time_idle();                                     /* (a fast-forward doesn't wait for a thread that's gone) */
    return NULL;
}

esp_err_t esp_wifi_connect(void)
{
    pthread_mutex_lock(&s_lock);
    if (!s_started) { pthread_mutex_unlock(&s_lock); return ESP_ERR_INVALID_STATE; }
    unsigned gen = ++s_gen;
    pthread_mutex_unlock(&s_lock);
    pthread_t t;
    pthread_create(&t, NULL, assoc_thread, (void *)(uintptr_t)gen);
    pthread_detach(t);
    return ESP_OK;
}

esp_err_t esp_wifi_disconnect(void)
{
    pthread_mutex_lock(&s_lock);
    int was = s_connected;
    s_connected = 0;
    s_gen++;
    memset(&s_sta.ip, 0, sizeof s_sta.ip);
    pthread_mutex_unlock(&s_lock);
    if (was) {
        wifi_event_sta_disconnected_t d = { .reason = 8 /* ASSOC_LEAVE */ };
        post(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &d, sizeof d);
    }
    return ESP_OK;
}

void sim_wifi_set_rssi(int dbm)
{
    pthread_mutex_lock(&s_lock);
    s_rssi = dbm < -100 ? -100 : dbm > -20 ? -20 : dbm;
    pthread_mutex_unlock(&s_lock);
}

esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t *ap)
{
    if (!ap) return ESP_ERR_INVALID_ARG;
    pthread_mutex_lock(&s_lock);
    int c = s_connected;
    if (c) {
        memset(ap, 0, sizeof *ap);
        memcpy(ap->ssid, s_cfg.sta.ssid, 32);
        ap->rssi = (int8_t)s_rssi;
        ap->primary = 6;
    }
    pthread_mutex_unlock(&s_lock);
    return c ? ESP_OK : ESP_ERR_NOT_FOUND;   /* real: ESP_ERR_WIFI_NOT_CONNECT */
}

/* ------------------------------------------------------- bind port map */

static uint16_t s_map_from, s_map_to, s_refuse;
static bool s_loopback;

void sim_net_refuse_port(uint16_t host_port) { s_refuse = host_port; }

void sim_net_loopback(bool on) { s_loopback = on; }

void sim_net_map_port(uint16_t fw_port, uint16_t host_port)
{
    s_map_from = fw_port;
    s_map_to = host_port;
}

static uint16_t map_port(uint16_t p) { return (s_map_from && p == s_map_from) ? s_map_to : p; }

uint16_t sim_net_host_port(uint16_t fw_port) { return map_port(fw_port); }

int mock_lwip_bind(int s, const struct sockaddr *name, socklen_t namelen)
{
    sim_net_forget(s);                                   /* (its number may be a closed connection's) */
    if (name && name->sa_family == AF_INET && namelen >= sizeof(struct sockaddr_in)) {
        struct sockaddr_in in = *(const struct sockaddr_in *)name;
        int type = 0;
        socklen_t tlen = sizeof type;
        getsockopt(s, SOL_SOCKET, SO_TYPE, (void *)&type, &tlen);
        /* (the mapped port is the control link's, TCP: a UDP socket on that number is another thing) */
        bool mapped = type == SOCK_STREAM && s_map_from && ntohs(in.sin_port) == s_map_from;
        if (s_loopback && in.sin_addr.s_addr == htonl(INADDR_ANY)) in.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (mapped) in.sin_port = htons(s_map_to);
        int r = bind(s, (const struct sockaddr *)&in, sizeof in);
        socklen_t len = sizeof in;
        if (r == 0 && mapped && s_map_to == 0 && getsockname(s, (struct sockaddr *)&in, &len) == 0) {
            s_map_to = ntohs(in.sin_port);           /* "any free port": keep the one we got, */
            printf("Port %u: 127.0.0.1:%u\n", (unsigned)s_map_from, (unsigned)s_map_to);   /* and say which (tests) */
            fflush(stdout);
        }
        return r;
    }
    return bind(s, name, namelen);
}

int mock_lwip_connect(int s, const struct sockaddr *name, socklen_t namelen)
{
    if (name && name->sa_family == AF_INET && namelen >= sizeof(struct sockaddr_in)) {
        struct sockaddr_in in = *(const struct sockaddr_in *)name;
        uint32_t a = ntohl(in.sin_addr.s_addr);
        bool here = a == INADDR_ANY || (a >> 24) == 127;
        if (s_refuse && ntohs(in.sin_port) == s_refuse && here) {
            errno = ECONNREFUSED;
            return -1;
        }
        if (s_map_from && ntohs(in.sin_port) == s_map_from && here) {
            in.sin_port = htons(s_map_to);
            return connect(s, (const struct sockaddr *)&in, sizeof in);
        }
    }
    sim_net_client(s);
    return connect(s, name, namelen);
}

/* ------------------------------------------------------------------ mDNS */

/* Announced through this computer's mDNS (host.h: Bonjour on macOS, Avahi's
   Bonjour API on Linux if installed, DNS-SD on Windows). */
static char s_instance[64] = "luaboard";

esp_err_t mdns_init(void) { return ESP_OK; }
esp_err_t mdns_hostname_set(const char *h) { (void)h; return ESP_OK; } /* the computer keeps its own name */

esp_err_t mdns_instance_name_set(const char *n)
{
    snprintf(s_instance, sizeof s_instance, "%s", n ? n : "luaboard");
    return ESP_OK;
}

esp_err_t mdns_service_add(const char *instance, const char *type, const char *proto,
                           uint16_t port, mdns_txt_item_t txt[], size_t n)
{
    char regtype[64], kv[8][160];
    const char *items[8];
    snprintf(regtype, sizeof regtype, "%s.%s", type, proto);
    if (n > 8) n = 8;
    for (size_t i = 0; i < n; i++) {
        snprintf(kv[i], sizeof kv[i], "%s=%s", txt[i].key, txt[i].value);
        items[i] = kv[i];
    }
    if (!host_mdns_announce(instance ? instance : s_instance, regtype, map_port(port), items, n)) {
        ESP_LOGW("mdns", "no mDNS on this computer: find the simulator by address");
        return ESP_FAIL;
    }
    return ESP_OK;
}

void mdns_free(void) { host_mdns_withdraw(); }
