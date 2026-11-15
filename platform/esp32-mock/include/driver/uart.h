/* ESP32 mock platform - subset of ESP-IDF <driver/uart.h>. */
#pragma once

#include <stdbool.h>

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { UART_NUM_0 = 0, UART_NUM_1, UART_NUM_2, UART_NUM_MAX } uart_port_t;

typedef enum { UART_DATA_5_BITS, UART_DATA_6_BITS, UART_DATA_7_BITS, UART_DATA_8_BITS } uart_word_length_t;
typedef enum { UART_PARITY_DISABLE = 0, UART_PARITY_EVEN = 2, UART_PARITY_ODD = 3 } uart_parity_t;
typedef enum { UART_STOP_BITS_1 = 1, UART_STOP_BITS_1_5 = 2, UART_STOP_BITS_2 = 3 } uart_stop_bits_t;
typedef enum { UART_HW_FLOWCTRL_DISABLE = 0 } uart_hw_flowcontrol_t;
typedef enum { UART_SCLK_DEFAULT = 0 } uart_sclk_t;

#define UART_PIN_NO_CHANGE (-1)

typedef struct {
    int baud_rate;
    uart_word_length_t data_bits;
    uart_parity_t parity;
    uart_stop_bits_t stop_bits;
    uart_hw_flowcontrol_t flow_ctrl;
    uint8_t rx_flow_ctrl_thresh;
    uart_sclk_t source_clk;
} uart_config_t;

typedef void *QueueHandle_t;

esp_err_t uart_driver_install(uart_port_t port, int rx_buffer_size, int tx_buffer_size,
                              int queue_size, QueueHandle_t *uart_queue, int intr_alloc_flags);
esp_err_t uart_param_config(uart_port_t port, const uart_config_t *cfg);
esp_err_t uart_set_pin(uart_port_t port, int tx, int rx, int rts, int cts);
/** Returns number of bytes read (0..length) or -1 on error. */
int uart_read_bytes(uart_port_t port, void *buf, uint32_t length, TickType_t ticks_to_wait);
/** Returns number of bytes queued for transmission or -1 on error. */
int uart_write_bytes(uart_port_t port, const void *src, size_t size);
esp_err_t uart_driver_delete(uart_port_t port);
bool uart_is_driver_installed(uart_port_t port);
/** Room left in the TX buffer. */
esp_err_t uart_get_tx_buffer_free_size(uart_port_t port, size_t *size);
/** Wait until everything written has left the wire; ESP_ERR_TIMEOUT if not by then. */
esp_err_t uart_wait_tx_done(uart_port_t port, TickType_t ticks_to_wait);

#ifdef __cplusplus
}
#endif
