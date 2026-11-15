#include "esp_netif_sntp.h"

#include <stdio.h>
#include <sys/time.h>

#include "sim_hw.h"

static esp_sntp_time_cb_t s_cb;

esp_err_t esp_netif_sntp_start(void)
{
    /* The network's time is the world's: the system clock is set to it. */
    sim_system_clock_sync();
    int64_t t = sim_world_us();
    struct timeval tv = { .tv_sec = (time_t)(t / 1000000), .tv_usec = (suseconds_t)(t % 1000000) };
    if (s_cb) s_cb(&tv);
    return ESP_OK;
}

esp_err_t esp_netif_sntp_init(const esp_sntp_config_t *config)
{
    fprintf(stderr, "[sim] SNTP %s: the simulated world's clock\n", config->num_of_servers ? config->servers[0] : "-");
    s_cb = config->sync_cb;
    return esp_netif_sntp_start();
}

void esp_netif_sntp_deinit(void) {}
