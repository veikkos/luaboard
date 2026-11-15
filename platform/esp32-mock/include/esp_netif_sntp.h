/* Mock of ESP-IDF's esp_netif_sntp.h: the simulator takes the host's clock,
   "synced" as soon as SNTP starts (the firmware starts it once Wi-Fi is up). */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <sys/time.h>
#include "esp_err.h"

typedef void (*esp_sntp_time_cb_t)(struct timeval *tv);

typedef struct {
    bool smooth_sync;
    bool server_from_dhcp;
    bool wait_for_sync;
    bool start;
    esp_sntp_time_cb_t sync_cb;
    size_t num_of_servers;
    const char *servers[1];
} esp_sntp_config_t;

#define ESP_NETIF_SNTP_DEFAULT_CONFIG(server) \
    { .wait_for_sync = true, .start = true, .num_of_servers = 1, .servers = { server } }

esp_err_t esp_netif_sntp_init(const esp_sntp_config_t *config);
/** Start, or restart (ask the server now). */
esp_err_t esp_netif_sntp_start(void);
void esp_netif_sntp_deinit(void);
