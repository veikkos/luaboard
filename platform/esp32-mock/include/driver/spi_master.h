/* ESP32 mock platform - subset of ESP-IDF <driver/spi_master.h>. */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { SPI1_HOST = 0, SPI2_HOST = 1, SPI3_HOST = 2 } spi_host_device_t;
typedef enum { SPI_DMA_DISABLED = 0, SPI_DMA_CH_AUTO = 3 } spi_dma_chan_t;

typedef struct {
    int mosi_io_num;
    int miso_io_num;
    int sclk_io_num;
    int quadwp_io_num;
    int quadhd_io_num;
    int max_transfer_sz;
    uint32_t flags;
} spi_bus_config_t;

typedef struct spi_transaction_t spi_transaction_t;
typedef void (*transaction_cb_t)(spi_transaction_t *trans);

typedef struct {
    uint8_t command_bits, address_bits, dummy_bits;
    uint8_t mode;
    int clock_speed_hz;
    int spics_io_num;
    uint32_t flags;
    int queue_size;
    transaction_cb_t pre_cb;
    transaction_cb_t post_cb;
} spi_device_interface_config_t;

#define SPI_TRANS_USE_RXDATA (1 << 2)
#define SPI_TRANS_USE_TXDATA (1 << 3)

struct spi_transaction_t {
    uint32_t flags;
    uint16_t cmd;
    uint64_t addr;
    size_t length;      /* total data length, in bits */
    size_t rxlength;
    void *user;
    union {
        const void *tx_buffer;
        uint8_t tx_data[4];
    };
    union {
        void *rx_buffer;
        uint8_t rx_data[4];
    };
};

typedef struct spi_device_t *spi_device_handle_t;

esp_err_t spi_bus_initialize(spi_host_device_t host, const spi_bus_config_t *bus_config,
                             spi_dma_chan_t dma_chan);
esp_err_t spi_bus_add_device(spi_host_device_t host, const spi_device_interface_config_t *dev_config,
                             spi_device_handle_t *handle);
esp_err_t spi_device_polling_transmit(spi_device_handle_t handle, spi_transaction_t *trans);
esp_err_t spi_device_transmit(spi_device_handle_t handle, spi_transaction_t *trans);

#ifdef __cplusplus
}
#endif
