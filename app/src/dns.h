/* Non-blocking name lookups: getaddrinfo runs on a worker thread. */
#pragma once

#include "lwip/sockets.h"

#define DNS_SLOTS    20    /* lua_net's 16 sockets plus room for httpc */
#define DNS_HOST_MAX 128   /* size of a host name buffer, with the NUL */

/** Queue a lookup; its slot, or -1 if none is free (or no thread). */
int dns_start(const char *host);
/** The answer for a slot: 0 still looking, 1 found (*addr), -1 not found.
    A finished slot is freed. */
int dns_poll(int slot, struct in_addr *addr);
/** The caller is gone: a queued lookup is dropped, a running one ignored. */
void dns_abandon(int slot);
