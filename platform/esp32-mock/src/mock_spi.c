/*
 * ESP32 mock platform - SPI master.
 * Each device is identified by its CS pin; transactions are delivered to the
 * device model the simulator attached to that CS pin. Models that need the
 * D/C line (e.g. LCD controllers) read it via sim_gpio_output_level().
 */
#include <stdlib.h>

#include "driver/spi_master.h"
#include "sim_hw.h"

#define MAX_MODELS 4

typedef struct {
    int cs;
    sim_spi_write_fn fn;
    void *ctx;
} model_t;

struct spi_device_t {
    spi_host_device_t host;
    spi_device_interface_config_t cfg;
};

static model_t s_models[MAX_MODELS];
static int s_nmodels;
static int s_bus_ready[3];
static int s_bus_miso[3] = { -1, -1, -1 };

void sim_spi_attach(int cs_pin, sim_spi_write_fn on_write, void *ctx)
{
    if (s_nmodels < MAX_MODELS) s_models[s_nmodels++] = (model_t){ cs_pin, on_write, ctx };
}

esp_err_t spi_bus_initialize(spi_host_device_t host, const spi_bus_config_t *bus_config,
                             spi_dma_chan_t dma_chan)
{
    (void)dma_chan;
    if (host < SPI1_HOST || host > SPI3_HOST || !bus_config) return ESP_ERR_INVALID_ARG;
    if (s_bus_ready[host]) return ESP_ERR_INVALID_STATE;
    s_bus_ready[host] = 1;
    s_bus_miso[host] = bus_config->miso_io_num;
    return ESP_OK;
}

/* The bus's MISO pin, -1 if it has none (or isn't set up): for the SD card mock. */
int mock_spi_bus_miso(spi_host_device_t host);
int mock_spi_bus_miso(spi_host_device_t host)
{
    return host >= SPI1_HOST && host <= SPI3_HOST && s_bus_ready[host] ? s_bus_miso[host] : -1;
}

esp_err_t spi_bus_add_device(spi_host_device_t host, const spi_device_interface_config_t *dev_config,
                             spi_device_handle_t *handle)
{
    if (host < SPI1_HOST || host > SPI3_HOST || !dev_config || !handle) return ESP_ERR_INVALID_ARG;
    if (!s_bus_ready[host]) return ESP_ERR_INVALID_STATE;
    struct spi_device_t *d = calloc(1, sizeof *d);
    if (!d) return ESP_ERR_NO_MEM;
    d->host = host;
    d->cfg = *dev_config;
    *handle = d;
    return ESP_OK;
}

esp_err_t spi_device_polling_transmit(spi_device_handle_t h, spi_transaction_t *t)
{
    if (!h || !t || t->length % 8) return ESP_ERR_INVALID_ARG;
    if (t->length > 4 * 8 && (t->flags & SPI_TRANS_USE_TXDATA)) return ESP_ERR_INVALID_ARG;
    const uint8_t *data = (t->flags & SPI_TRANS_USE_TXDATA) ? t->tx_data : t->tx_buffer;
    if (h->cfg.pre_cb) h->cfg.pre_cb(t);
    for (int i = 0; i < s_nmodels; i++)
        if (s_models[i].cs == h->cfg.spics_io_num && data)
            s_models[i].fn(s_models[i].ctx, data, t->length / 8);
    if (h->cfg.post_cb) h->cfg.post_cb(t);
    return ESP_OK;
}

esp_err_t spi_device_transmit(spi_device_handle_t h, spi_transaction_t *t)
{
    return spi_device_polling_transmit(h, t);
}
