/*
 * Mock of ESP-IDF's certificate bundle (components/mbedtls/esp_crt_bundle).
 * On the device the bundle is a set of trusted root CAs built into the
 * firmware. Here it is the host's root store (macOS: /etc/ssl/cert.pem;
 * override with the SIM_CA_FILE environment variable).
 */
#pragma once

#include "esp_err.h"

/** Make an mbedtls_ssl_config verify servers against the bundle. */
esp_err_t esp_crt_bundle_attach(void *conf);
