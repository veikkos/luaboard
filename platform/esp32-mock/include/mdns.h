/* ESP32 mock platform - subset of the espressif/mdns component API.
 * In the simulator the service is announced with macOS Bonjour (dns_sd). */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct { const char *key; const char *value; } mdns_txt_item_t;

esp_err_t mdns_init(void);
void mdns_free(void);
esp_err_t mdns_hostname_set(const char *hostname);
esp_err_t mdns_instance_name_set(const char *instance_name);
esp_err_t mdns_service_add(const char *instance_name, const char *service_type, const char *proto,
                           uint16_t port, mdns_txt_item_t txt[], size_t num_items);

#ifdef __cplusplus
}
#endif
