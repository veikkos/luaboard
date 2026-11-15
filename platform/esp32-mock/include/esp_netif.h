/* ESP32 mock platform - subset of ESP-IDF <esp_netif.h> (+ IP events). */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_event.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct { uint32_t addr; } esp_ip4_addr_t;   /* network byte order */
typedef struct { esp_ip4_addr_t ip, netmask, gw; } esp_netif_ip_info_t;
typedef struct esp_netif_obj esp_netif_t;

#define IPSTR "%d.%d.%d.%d"
#define esp_ip4_addr_get_byte(a, n) (((const uint8_t *)(&(a)->addr))[n])
#define IP2STR(a) esp_ip4_addr_get_byte(a, 0), esp_ip4_addr_get_byte(a, 1), \
                  esp_ip4_addr_get_byte(a, 2), esp_ip4_addr_get_byte(a, 3)

ESP_EVENT_DECLARE_BASE(IP_EVENT);
typedef enum { IP_EVENT_STA_GOT_IP = 0, IP_EVENT_STA_LOST_IP = 1 } ip_event_t;
typedef struct {
    esp_netif_t *esp_netif;
    esp_netif_ip_info_t ip_info;
    bool ip_changed;
} ip_event_got_ip_t;

esp_err_t esp_netif_init(void);
esp_netif_t *esp_netif_create_default_wifi_sta(void);
esp_err_t esp_netif_set_hostname(esp_netif_t *netif, const char *hostname);
esp_err_t esp_netif_get_ip_info(esp_netif_t *netif, esp_netif_ip_info_t *ip_info);

#ifdef __cplusplus
}
#endif
