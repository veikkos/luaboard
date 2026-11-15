/*
 * ESP32 mock platform - subset of ESP-IDF <esp_err.h>.
 * Signatures follow ESP-IDF v5.x so application code builds against both.
 */
#pragma once

#include <stdio.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int esp_err_t;

#define ESP_OK                    0
#define ESP_FAIL                 -1
#define ESP_ERR_NO_MEM           0x101
#define ESP_ERR_INVALID_ARG      0x102
#define ESP_ERR_INVALID_STATE    0x103
#define ESP_ERR_INVALID_SIZE     0x104
#define ESP_ERR_NOT_FOUND        0x105
#define ESP_ERR_NOT_SUPPORTED    0x106
#define ESP_ERR_TIMEOUT          0x107

#define ESP_ERR_NVS_BASE         0x1100
#define ESP_ERR_NVS_NOT_FOUND    (ESP_ERR_NVS_BASE + 0x02)
#define ESP_ERR_NVS_NOT_ENOUGH_SPACE (ESP_ERR_NVS_BASE + 0x05)
#define ESP_ERR_NVS_INVALID_LENGTH (ESP_ERR_NVS_BASE + 0x0c)

const char *esp_err_to_name(esp_err_t code);

#define ESP_ERROR_CHECK(x) do {                                              \
        esp_err_t err_rc_ = (x);                                             \
        if (err_rc_ != ESP_OK) {                                             \
            fprintf(stderr, "ESP_ERROR_CHECK failed: %s (0x%x) at %s:%d\n",  \
                    esp_err_to_name(err_rc_), err_rc_, __FILE__, __LINE__);  \
            abort();                                                         \
        }                                                                    \
    } while (0)

#ifdef __cplusplus
}
#endif
