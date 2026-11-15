/* ESP32 mock platform - subset of ESP-IDF <driver/usb_serial_jtag.h>.
 * The ESP32-S3's built-in USB CDC port (no external USB-UART bridge). */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t tx_buffer_size;
    uint32_t rx_buffer_size;
} usb_serial_jtag_driver_config_t;

#define USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT() { .tx_buffer_size = 256, .rx_buffer_size = 256 }

esp_err_t usb_serial_jtag_driver_install(usb_serial_jtag_driver_config_t *cfg);
/** Returns number of bytes read (0..length). */
int usb_serial_jtag_read_bytes(void *buf, uint32_t length, TickType_t ticks_to_wait);
/** Returns number of bytes written. */
int usb_serial_jtag_write_bytes(const void *src, size_t size, TickType_t ticks_to_wait);
/** True while a USB host is attached (on the device: start-of-frame packets seen). */
bool usb_serial_jtag_is_connected(void);

#ifdef __cplusplus
}
#endif
