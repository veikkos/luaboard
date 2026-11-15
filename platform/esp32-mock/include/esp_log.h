/*
 * ESP32 mock platform - subset of ESP-IDF <esp_log.h>.
 * In the simulator logs go to the host's stderr (NOT the simulated UART),
 * so they never interfere with the serial protocol.
 */
#pragma once

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

void esp_mock_log(char level, const char *tag, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

#define ESP_LOGE(tag, fmt, ...) esp_mock_log('E', tag, fmt, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) esp_mock_log('W', tag, fmt, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) esp_mock_log('I', tag, fmt, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) esp_mock_log('D', tag, fmt, ##__VA_ARGS__)

#ifdef __cplusplus
}
#endif
