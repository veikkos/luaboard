/*
 * ESP32 mock platform - I2C master (new v5 driver API).
 * Transactions are delivered to device models attached by the simulator.
 */
#include <stdlib.h>

#include "driver/i2c_master.h"
#include "sim_hw.h"

#define MAX_MODELS 12

typedef struct {
    int port;
    uint8_t addr;
    sim_i2c_write_fn wr;
    sim_i2c_read_fn rd;
    void *ctx;
} model_t;

struct i2c_master_bus_t { i2c_master_bus_config_t cfg; };
struct i2c_master_dev_t { i2c_master_bus_handle_t bus; i2c_device_config_t cfg; };

static model_t s_models[MAX_MODELS];
static int s_nmodels;

void sim_i2c_attach_port(int port, uint8_t addr7, sim_i2c_write_fn on_write, sim_i2c_read_fn on_read, void *ctx)
{
    if (s_nmodels < MAX_MODELS) s_models[s_nmodels++] = (model_t){ port, addr7, on_write, on_read, ctx };
}

void sim_i2c_attach(uint8_t addr7, sim_i2c_write_fn on_write, sim_i2c_read_fn on_read, void *ctx)
{
    sim_i2c_attach_port(0, addr7, on_write, on_read, ctx);
}

void sim_i2c_detach_port(int port, uint8_t addr7)
{
    for (int i = 0; i < s_nmodels; i++)
        if (s_models[i].port == port && s_models[i].addr == addr7) { s_models[i] = s_models[--s_nmodels]; return; }
}

static model_t *find_at(int port, uint16_t addr)
{
    for (int i = 0; i < s_nmodels; i++)
        if (s_models[i].port == port && s_models[i].addr == addr) return &s_models[i];
    return NULL;
}

bool sim_i2c_present(int port, uint8_t addr7) { return find_at(port, addr7) != NULL; }

static model_t *find_model(i2c_master_dev_handle_t dev)
{
    return find_at((int)dev->bus->cfg.i2c_port, dev->cfg.device_address);
}

esp_err_t i2c_del_master_bus(i2c_master_bus_handle_t bus_handle)
{
    if (!bus_handle) return ESP_ERR_INVALID_ARG;
    free(bus_handle);
    return ESP_OK;
}

esp_err_t i2c_master_probe(i2c_master_bus_handle_t bus_handle, uint16_t address, int xfer_timeout_ms)
{
    (void)xfer_timeout_ms;
    if (!bus_handle) return ESP_ERR_INVALID_ARG;
    return find_at((int)bus_handle->cfg.i2c_port, address) ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t i2c_new_master_bus(const i2c_master_bus_config_t *bus_config,
                             i2c_master_bus_handle_t *ret_bus_handle)
{
    if (!bus_config || !ret_bus_handle) return ESP_ERR_INVALID_ARG;
    struct i2c_master_bus_t *bus = calloc(1, sizeof(*bus));
    if (!bus) return ESP_ERR_NO_MEM;
    bus->cfg = *bus_config;
    *ret_bus_handle = bus;
    return ESP_OK;
}

esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t bus_handle,
                                    const i2c_device_config_t *dev_config,
                                    i2c_master_dev_handle_t *ret_handle)
{
    if (!bus_handle || !dev_config || !ret_handle) return ESP_ERR_INVALID_ARG;
    struct i2c_master_dev_t *dev = calloc(1, sizeof(*dev));
    if (!dev) return ESP_ERR_NO_MEM;
    dev->bus = bus_handle;
    dev->cfg = *dev_config;
    *ret_handle = dev;
    return ESP_OK;
}

esp_err_t i2c_master_bus_rm_device(i2c_master_dev_handle_t handle)
{
    if (!handle) return ESP_ERR_INVALID_ARG;
    free(handle);
    return ESP_OK;
}

esp_err_t i2c_master_transmit(i2c_master_dev_handle_t dev, const uint8_t *write_buffer,
                              size_t write_size, int xfer_timeout_ms)
{
    (void)xfer_timeout_ms;
    if (!dev || (!write_buffer && write_size)) return ESP_ERR_INVALID_ARG;
    model_t *m = find_model(dev);
    if (!m) return ESP_FAIL; /* NACK: nothing at this address */
    if (m->wr) m->wr(m->ctx, write_buffer, write_size);
    return ESP_OK;
}

esp_err_t i2c_master_receive(i2c_master_dev_handle_t dev, uint8_t *read_buffer, size_t read_size,
                             int xfer_timeout_ms)
{
    (void)xfer_timeout_ms;
    if (!dev || !read_buffer) return ESP_ERR_INVALID_ARG;
    model_t *m = find_model(dev);
    if (!m || !m->rd) return ESP_FAIL;
    m->rd(m->ctx, read_buffer, read_size);
    return ESP_OK;
}

esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t dev, const uint8_t *write_buffer,
                                      size_t write_size, uint8_t *read_buffer, size_t read_size,
                                      int xfer_timeout_ms)
{
    esp_err_t err = i2c_master_transmit(dev, write_buffer, write_size, xfer_timeout_ms);
    if (err != ESP_OK) return err;
    return i2c_master_receive(dev, read_buffer, read_size, xfer_timeout_ms);
}
