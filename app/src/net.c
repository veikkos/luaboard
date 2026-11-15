#include "net.h"
#include "ota.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "app_config.h"
#include "auth.h"
#include "board.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "hex.h"
#include "hw.h"
#include "lwip/sockets.h"
#include "mdns.h"
#include "nvs.h"
#include "clock.h"
#include "lua_net.h"
#include "protocol.h"
#include "settings.h"
#include "wifilink.h"

static const char *TAG = "net";

typedef enum { NET_OFF, NET_CONNECTING, NET_CONNECTED } net_state_t;

/* Set from the event-loop task, consumed by the main loop. */
static atomic_int s_ev_start, s_ev_got_ip, s_ev_lost;
static atomic_int s_last_reason;

static bool s_wifi_ready, s_wifi_started;
static net_state_t s_state = NET_OFF;
static esp_netif_t *s_netif;
static char s_ssid[33];
static char s_ip[16] = "-";
static int s_retries;
static uint32_t s_retry_at;
static bool s_retry_pending;

/* Wi-Fi clients. At most one is logged in; the others are still logging in.
   A new connection only ever pushes out a client that hasn't logged in, so
   someone on the network without the key can't kick a working session. A
   logged-in client is replaced when a newer client logs in (a PC that
   reconnects after a network blip beats its own half-dead connection).
   Logged in = one of its frames decrypted (wifilink.c). */
typedef struct {
    int fd;               /* -1: slot free */
    int link;             /* protocol link id */
    uint32_t since;       /* connected at (ms) */
    bool authed;          /* logged in: the other clients were sent away */
    wl_t wl;              /* the encrypted link */
} client_t;

_Static_assert(NET_MAX_CLIENTS + 1 <= PROTO_MAX_LINKS, "a protocol link per Wi-Fi client, plus USB");

/* fd -1 from the start: before wifi_setup() no slot is a connection. (One
   initializer per client: standard C has no range designators.) */
_Static_assert(NET_MAX_CLIENTS == 3, "one { .fd = -1 } per client below");
static client_t s_clients[NET_MAX_CLIENTS] = { { .fd = -1 }, { .fd = -1 }, { .fd = -1 } };
static int s_listen = -1;
static bool s_mdns_up;

/* ------------------------------------------------------------- NVS creds */

static void nvs_str(const char *key, char *out, size_t cap) { out[settings_load(key, out, cap - 1)] = 0; }

/* --------------------------------------------------------------- events */

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        atomic_store(&s_ev_start, 1);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        atomic_store(&s_last_reason, ((wifi_event_sta_disconnected_t *)data)->reason);
        atomic_store(&s_ev_lost, 1);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        atomic_store(&s_ev_got_ip, 1);
    }
}

/* ------------------------------------------------------------ TCP clients */

static void close_client(client_t *c, const char *why)
{
    if (c->fd < 0) return;
    ESP_LOGI(TAG, "client closed (%s)", why);
    close(c->fd);
    c->fd = -1;
    c->authed = false;
    wl_close(&c->wl);
    proto_link_reset(c->link);
}

static void close_all_clients(const char *why)
{
    for (int i = 0; i < NET_MAX_CLIENTS; i++) close_client(&s_clients[i], why);
}

static void say_bye(client_t *c, const char *why);

void net_drop_clients(const char *why)
{
    for (int i = 0; i < NET_MAX_CLIENTS; i++)
        if (s_clients[i].fd >= 0) say_bye(&s_clients[i], why);
}

/* Bytes as they are on the wire (frames, or the clear-text HELLO lines). */
static void raw_send(void *ctx, const void *data, size_t len);

static void say_bye(client_t *c, const char *why)
{
    char bye[96];
    int n = snprintf(bye, sizeof bye, "EVT bye %s\n", why);
    if (c->wl.state == WL_SECURE) {
        if (proto_link_authed(c->link)) wl_write(&c->wl, bye, (size_t)n, raw_send, c);
    } else {
        send(c->fd, bye, (size_t)n, MSG_DONTWAIT);      /* still in HELLO: clear text */
    }
    close_client(c, why);
}

/* A client has proved it has the key: the other clients go (the newest
   login wins). Done the moment it happens: a client that logs in, gets its
   reply and hangs up within one pass of the main loop replaces the old one
   as any other does. */
static void logged_in_now(client_t *c)
{
    proto_link_login(c->link);
    c->authed = true;
    for (int i = 0; i < NET_MAX_CLIENTS; i++)
        if (&s_clients[i] != c && s_clients[i].fd >= 0) say_bye(&s_clients[i], "another client logged in");
}

/* The protocol's reads: decrypted bytes. */
static int tcp_read(void *ctx, uint8_t *buf, size_t cap)
{
    client_t *c = ctx;
    for (int round = 0; c->fd >= 0 && round < 8; round++) {
        int got = wl_read(&c->wl, buf, cap);
        if (got < 0) { close_client(c, "a frame didn't decrypt"); return 0; }
        if (got > 0) {
            if (c->wl.verified && !c->authed) logged_in_now(c);
            return got;
        }
        size_t room;
        uint8_t *dst = wl_room(&c->wl, &room);
        if (!room) return 0;
        ssize_t n = recv(c->fd, dst, room, MSG_DONTWAIT);
        if (n > 0) {
            if (!wl_received(&c->wl, (size_t)n, raw_send, c)) { close_client(c, "no valid HELLO"); return 0; }
            continue;
        }
        if (n == 0) close_client(c, "peer closed");
        else if (errno != EAGAIN && errno != EWOULDBLOCK) close_client(c, "recv error");
        return 0;
    }
    return 0;
}

/* The protocol's writes: encrypted. */
static void tcp_write(void *ctx, const void *data, size_t len)
{
    client_t *c = ctx;
    if (c->fd >= 0) wl_write(&c->wl, data, len, raw_send, c);
}

static void raw_send(void *ctx, const void *data, size_t len)
{
    client_t *c = ctx;
    const uint8_t *p = data;
    while (len > 0 && c->fd >= 0) {
        ssize_t n = send(c->fd, p, len, 0);
        if (n > 0) { p += n; len -= (size_t)n; continue; }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            /* Only a logged-in client is worth stalling the device for. One
               without the key that doesn't read its replies is dropped. */
            if (!proto_link_authed(c->link)) { close_client(c, "client stopped reading"); break; }
            struct pollfd pfd = { .fd = c->fd, .events = POLLOUT };
            if (poll(&pfd, 1, NET_SEND_STALL_MS) > 0) continue;
            close_client(c, "client stopped reading");
        } else {
            close_client(c, "send error");
        }
    }
}

static void start_listener(void)
{
    if (s_listen >= 0) return;
    int s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s < 0) { ESP_LOGE(TAG, "socket: %d", errno); return; }
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(NET_TCP_PORT),
                             .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (bind(s, (struct sockaddr *)&a, sizeof a) != 0 || listen(s, NET_MAX_CLIENTS) != 0) {
        ESP_LOGE(TAG, "cannot listen on port %d (errno %d)", NET_TCP_PORT, errno);
        close(s);
        return;
    }
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
    s_listen = s;
    ESP_LOGI(TAG, "listening on port %d", NET_TCP_PORT);
}

static void start_mdns(void)
{
    if (s_mdns_up) return;
    if (mdns_init() != ESP_OK) return;
    mdns_hostname_set(NET_HOSTNAME);
    mdns_instance_name_set(NET_HOSTNAME);
    /* auth reflects the setting at announcement time; clients also learn it
       from the HELLO reply. */
    char api[12];
    snprintf(api, sizeof api, "%d", APP_LUA_API);
    mdns_txt_item_t txt[] = { { "board", BOARD_NAME }, { "fw", fw_version() }, { "api", api },
                              { "auth", auth_paired() ? "key" : "none" } };
    if (mdns_service_add(NULL, NET_MDNS_SERVICE, "_tcp", NET_TCP_PORT, txt, 4) == ESP_OK) s_mdns_up = true;
}

static void accept_client(void)
{
    if (s_listen < 0) return;
    struct sockaddr_in peer;
    socklen_t plen = sizeof peer;
    int fd = accept(s_listen, (struct sockaddr *)&peer, &plen);
    if (fd < 0) return;
    /* A script on this device dialling its own control port: apps must not
       drive the device (install code into other apps, change its settings). */
    if (lua_net_is_own_socket(&peer)) {
        ESP_LOGW(TAG, "refused a connection from a script on this device");
        close(fd);
        return;
    }
    /* A free slot, else the oldest client that hasn't logged in. Never the
       logged-in one: there is at most one, and NET_MAX_CLIENTS > 1. */
    client_t *slot = NULL;
    for (int i = 0; i < NET_MAX_CLIENTS && !slot; i++) if (s_clients[i].fd < 0) slot = &s_clients[i];
    if (!slot)
        for (int i = 0; i < NET_MAX_CLIENTS; i++) {
            client_t *c = &s_clients[i];
            if (!proto_link_authed(c->link) && (!slot || (int32_t)(c->since - slot->since) < 0)) slot = c;
        }
    if (!slot) { close(fd); return; }               /* can't happen; be safe */
    if (slot->fd >= 0) say_bye(slot, "another client connected");
    if (!wl_open(&slot->wl)) { ESP_LOGW(TAG, "no memory for a client"); close(fd); return; }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    slot->fd = fd;
    slot->since = hw_millis();
    slot->authed = false;
    proto_link_reset(slot->link);
    ESP_LOGI(TAG, "client connected");
}

/* Clients that never log in time out. */
static void check_clients(void)
{
    uint32_t now = hw_millis();
    for (int i = 0; i < NET_MAX_CLIENTS; i++) {
        client_t *c = &s_clients[i];
        if (c->fd >= 0 && !proto_link_authed(c->link) && now - c->since > NET_LOGIN_TIMEOUT_MS)
            say_bye(c, "no login in time");
    }
}

/* ------------------------------------------------------------------ wifi */

static esp_err_t wifi_setup(void)
{
    if (s_wifi_ready) return ESP_OK;
    esp_err_t err = esp_netif_init();
    if (err == ESP_OK) err = esp_event_loop_create_default();
    if (err != ESP_OK) return err;
    s_netif = esp_netif_create_default_wifi_sta();
    esp_netif_set_hostname(s_netif, NET_HOSTNAME);
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    /* The SSID and password live in our own NVS keys; ESP-IDF would keep a
       second copy in its "nvs.net80211" namespace, which WIFI OFF can't see. */
    if (err == ESP_OK) err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err == ESP_OK) err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL, NULL);
    if (err == ESP_OK) err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL, NULL);
    if (err == ESP_OK) err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) return err;
    for (int i = 0; i < NET_MAX_CLIENTS; i++) {
        s_clients[i].fd = -1;
        s_clients[i].link = proto_add_link(&(proto_link_t){ .name = "wifi", .trusted = false, .read = tcp_read,
                                                            .write = tcp_write, .ctx = &s_clients[i] });
    }
    s_wifi_ready = true;
    /* Hold the control port from the start: no script may bind it first and
       pose as the device (net.listen refuses it too). */
    start_listener();
    return ESP_OK;
}

static esp_err_t wifi_start(const char *ssid, const char *pass)
{
    esp_err_t err = wifi_setup();
    if (err != ESP_OK) return err;
    wifi_config_t wc = { 0 };
    snprintf((char *)wc.sta.ssid, sizeof wc.sta.ssid, "%s", ssid);
    snprintf((char *)wc.sta.password, sizeof wc.sta.password, "%s", pass);
    wc.sta.threshold.authmode = *pass ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    snprintf(s_ssid, sizeof s_ssid, "%s", ssid);
    s_state = NET_CONNECTING;
    s_retries = 0;
    s_retry_pending = false;
    if (s_wifi_started) {
        esp_wifi_disconnect();
        err = esp_wifi_set_config(WIFI_IF_STA, &wc);
        if (err == ESP_OK) err = esp_wifi_connect();
        return err;
    }
    err = esp_wifi_set_config(WIFI_IF_STA, &wc);
    if (err == ESP_OK) err = esp_wifi_start();   /* -> STA_START -> connect from net_poll */
    if (err == ESP_OK) s_wifi_started = true;
    return err;
}

#ifndef ESP_PLATFORM
/* The simulator's own network (--set wifi_net=; sim_hw.h): what a simulated
   board joins while it has none set up, so a developer needn't set one. */
const char *sim_wifi_net(void);
#endif

void net_init(void)
{
    char ssid[33], pass[65];
    nvs_str(NVS_KEY_WIFI_SSID, ssid, sizeof ssid);
    nvs_str(NVS_KEY_WIFI_PASS, pass, sizeof pass);
#ifndef ESP_PLATFORM
    if (!ssid[0] && sim_wifi_net()) {
        snprintf(ssid, sizeof ssid, "%s", sim_wifi_net());
        pass[0] = 0;
        ESP_LOGI(TAG, "no Wi-Fi configured: the simulator's network %s", ssid);
    }
#endif
    if (!ssid[0]) { ESP_LOGI(TAG, "no Wi-Fi configured"); return; }
    esp_err_t err = wifi_start(ssid, pass);
    if (err != ESP_OK) ESP_LOGE(TAG, "Wi-Fi start failed: %s", esp_err_to_name(err));
}

/* This firmware keeps ESP-IDF's Wi-Fi config in RAM, but firmware flashed
   before it (a factory demo) may have left an SSID and password in ESP-IDF's
   own namespace. Erase that copy too when Wi-Fi is turned off. */
static void forget_idf_wifi_config(void)
{
    nvs_handle_t h;
    if (nvs_open("nvs.net80211", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_erase_all(h);
    nvs_commit(h);
    nvs_close(h);
}

esp_err_t net_set_credentials(const char *ssid, const char *pass)
{
    if (strlen(ssid) > 32 || strlen(pass) > 64) return ESP_ERR_INVALID_ARG;
    esp_err_t err = settings_store(NVS_KEY_WIFI_SSID, ssid, strlen(ssid));
    if (err == ESP_OK) err = settings_store(NVS_KEY_WIFI_PASS, pass, strlen(pass));
    if (err != ESP_OK) return err;

    if (!*ssid) {
        close_all_clients("Wi-Fi turned off");
        if (s_wifi_started) { esp_wifi_stop(); s_wifi_started = false; }
        forget_idf_wifi_config();
        s_state = NET_OFF;
        s_ssid[0] = 0;
        snprintf(s_ip, sizeof s_ip, "-");
        return ESP_OK;
    }
    return wifi_start(ssid, pass);
}

void net_poll(void)
{
    if (!s_wifi_ready) return;

    if (atomic_exchange(&s_ev_start, 0) && s_state == NET_CONNECTING) esp_wifi_connect();

    if (atomic_exchange(&s_ev_lost, 0) && s_state != NET_OFF) {
        bool was_connected = s_state == NET_CONNECTED;
        s_state = NET_CONNECTING;
        snprintf(s_ip, sizeof s_ip, "-");
        close_all_clients("Wi-Fi lost");
        /* Back off: 1, 2, 4, ... up to 30 s between attempts. */
        uint32_t delay = 1000u << (s_retries < 5 ? s_retries : 5);
        if (delay > 30000) delay = 30000;
        s_retries++;
        s_retry_at = hw_millis() + delay;
        s_retry_pending = true;
        if (was_connected) proto_eventf("wifi lost reason=%d", atomic_load(&s_last_reason));
    }
    if (s_retry_pending && (int32_t)(hw_millis() - s_retry_at) >= 0) {
        s_retry_pending = false;
        esp_wifi_connect();
    }

    if (atomic_exchange(&s_ev_got_ip, 0)) {
        esp_netif_ip_info_t ip;
        esp_netif_get_ip_info(s_netif, &ip);
        snprintf(s_ip, sizeof s_ip, IPSTR, IP2STR(&ip.ip));
        s_state = NET_CONNECTED;
        s_retries = 0;
        start_listener();
        start_mdns();
        proto_eventf("wifi connected ip=%s port=%d", s_ip, NET_TCP_PORT);
        clock_network_up();
    }

    if (s_state == NET_CONNECTED) accept_client();
    check_clients();
}

const char *net_ip(void) { return s_ip; }

bool net_wifi_failing(void) { return s_state == NET_CONNECTING && s_retries > 0; }

int net_rssi(void)
{
    wifi_ap_record_t ap;
    if (s_state != NET_CONNECTED || esp_wifi_sta_get_ap_info(&ap) != ESP_OK) return 0;
    return ap.rssi;
}

const char *net_state(void)
{
    static const char *names[] = { "off", "connecting", "connected" };
    return names[s_state];
}

static int logged_in(void)
{
    int n = 0;
    for (int i = 0; i < NET_MAX_CLIENTS; i++) n += s_clients[i].fd >= 0 && proto_link_authed(s_clients[i].link);
    return n;
}

void net_status(char *out, size_t len)
{
    static const char *names[] = { "off", "connecting", "connected" };
    char hex[67];
    size_t n = strlen(s_ssid);
    hex_encode(s_ssid, n, hex);
    int rssi = 0;
    wifi_ap_record_t ap;
    if (s_state == NET_CONNECTED && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) rssi = ap.rssi;
    snprintf(out, len, "state=%s ssid=%s ip=%s rssi=%d port=%d host=%s.local client=%d",
             names[s_state], n ? hex : "-", s_ip, rssi, NET_TCP_PORT, NET_HOSTNAME, logged_in());
}
