/* ESP32 mock platform - <lwip/netdb.h>. */
#pragma once
#ifdef _WIN32
#include <ws2tcpip.h>
#else
#include <netdb.h>
#endif

/* The computer's resolver, except for the names the simulator answers by
   itself (mock_wifi.c). */
int mock_lwip_getaddrinfo(const char *node, const char *service, const struct addrinfo *hints, struct addrinfo **res);
#define getaddrinfo(n, s, h, r) mock_lwip_getaddrinfo((n), (s), (h), (r))
