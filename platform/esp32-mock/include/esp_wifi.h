/* ESP32 mock platform - subset of ESP-IDF <esp_wifi.h> (station mode). */
#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "esp_event.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct { int magic; } wifi_init_config_t;
#define WIFI_INIT_CONFIG_DEFAULT() { .magic = 0x1F2F3F4F }

typedef enum { WIFI_MODE_NULL = 0, WIFI_MODE_STA, WIFI_MODE_AP, WIFI_MODE_APSTA } wifi_mode_t;
typedef enum { WIFI_IF_STA = 0, WIFI_IF_AP } wifi_interface_t;
typedef enum { WIFI_AUTH_OPEN = 0, WIFI_AUTH_WPA2_PSK = 3 } wifi_auth_mode_t;

typedef struct {
    uint8_t ssid[32];
    uint8_t password[64];
    struct { wifi_auth_mode_t authmode; } threshold;
} wifi_sta_config_t;

typedef union {
    wifi_sta_config_t sta;
} wifi_config_t;

typedef struct {
    uint8_t bssid[6];
    uint8_t ssid[33];
    uint8_t primary;
    int8_t rssi;
} wifi_ap_record_t;

ESP_EVENT_DECLARE_BASE(WIFI_EVENT);
typedef enum {
    WIFI_EVENT_STA_START = 2,
    WIFI_EVENT_STA_STOP = 3,
    WIFI_EVENT_STA_CONNECTED = 4,
    WIFI_EVENT_STA_DISCONNECTED = 5,
} wifi_event_t;

typedef struct {
    uint8_t ssid[32];
    uint8_t ssid_len;
    uint8_t bssid[6];
    uint8_t reason;     /* 8 ASSOC_LEAVE, 200 BEACON_TIMEOUT, 201 NO_AP_FOUND, 15 4WAY_HANDSHAKE_TIMEOUT */
    int8_t rssi;
} wifi_event_sta_disconnected_t;

typedef enum { WIFI_STORAGE_FLASH, WIFI_STORAGE_RAM } wifi_storage_t;

esp_err_t esp_wifi_init(const wifi_init_config_t *config);
esp_err_t esp_wifi_set_storage(wifi_storage_t storage);
esp_err_t esp_wifi_set_mode(wifi_mode_t mode);
esp_err_t esp_wifi_set_config(wifi_interface_t interface, wifi_config_t *conf);
esp_err_t esp_wifi_start(void);
esp_err_t esp_wifi_stop(void);
esp_err_t esp_wifi_connect(void);
esp_err_t esp_wifi_disconnect(void);
esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t *ap_info);

#ifdef __cplusplus
}
#endif
