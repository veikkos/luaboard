/* Name lookups block (lwIP's getaddrinfo waits for the DNS server, seconds
   for a name nobody answers). They run on a worker thread, one at a time;
   the caller keeps going and polls for the answer. */
#include "dns.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "lwip/netdb.h"

typedef enum { D_FREE, D_QUEUED, D_RUNNING, D_DONE } dstate_t;
typedef struct {
    dstate_t state;
    bool abandoned;       /* its owner is gone: drop the answer */
    char host[DNS_HOST_MAX];
    bool ok;
    struct in_addr addr;
} dns_req_t;

static dns_req_t s_dns[DNS_SLOTS];
static pthread_mutex_t s_dns_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_dns_wake = PTHREAD_COND_INITIALIZER;
static bool s_dns_thread;

static void *dns_worker(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&s_dns_lock);
    for (;;) {
        int i = 0;
        while (i < DNS_SLOTS && s_dns[i].state != D_QUEUED) i++;
        if (i == DNS_SLOTS) { pthread_cond_wait(&s_dns_wake, &s_dns_lock); continue; }
        dns_req_t *r = &s_dns[i];
        r->state = D_RUNNING;
        char host[sizeof r->host];
        memcpy(host, r->host, sizeof host);
        pthread_mutex_unlock(&s_dns_lock);

        struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM };
        struct addrinfo *res = NULL;
        bool ok = getaddrinfo(host, NULL, &hints, &res) == 0 && res;
        struct in_addr a = { 0 };
        if (ok) a = ((struct sockaddr_in *)res->ai_addr)->sin_addr;
        if (res) freeaddrinfo(res);

        pthread_mutex_lock(&s_dns_lock);
        if (r->abandoned) r->state = D_FREE;
        else { r->ok = ok; r->addr = a; r->state = D_DONE; }
    }
    return NULL;
}

int dns_start(const char *host)
{
    pthread_mutex_lock(&s_dns_lock);
    if (!s_dns_thread) {
        pthread_attr_t at;
        pthread_attr_init(&at);
#ifdef ESP_PLATFORM
        pthread_attr_setstacksize(&at, 4096);
#endif
        pthread_t t;
        s_dns_thread = pthread_create(&t, &at, dns_worker, NULL) == 0;
        if (s_dns_thread) pthread_detach(t);
        pthread_attr_destroy(&at);
    }
    int slot = -1;
    for (int i = 0; s_dns_thread && i < DNS_SLOTS && slot < 0; i++) if (s_dns[i].state == D_FREE) slot = i;
    if (slot >= 0) {
        dns_req_t *r = &s_dns[slot];
        snprintf(r->host, sizeof r->host, "%s", host);
        r->abandoned = false;
        r->state = D_QUEUED;
        pthread_cond_signal(&s_dns_wake);
    }
    pthread_mutex_unlock(&s_dns_lock);
    return slot;
}

int dns_poll(int slot, struct in_addr *addr)
{
    pthread_mutex_lock(&s_dns_lock);
    dns_req_t *r = &s_dns[slot];
    int rc = 0;
    if (r->state == D_DONE) {
        rc = r->ok ? 1 : -1;
        *addr = r->addr;
        r->state = D_FREE;
    }
    pthread_mutex_unlock(&s_dns_lock);
    return rc;
}

void dns_abandon(int slot)
{
    pthread_mutex_lock(&s_dns_lock);
    dns_req_t *r = &s_dns[slot];
    if (r->state == D_QUEUED || r->state == D_DONE) r->state = D_FREE;
    else if (r->state == D_RUNNING) r->abandoned = true;
    pthread_mutex_unlock(&s_dns_lock);
}
