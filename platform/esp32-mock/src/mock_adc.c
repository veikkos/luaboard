/*
 * ESP32 mock platform - ADC oneshot driver.
 * Values come from the simulator (sim_adc_set_raw); a little noise is added
 * so readings look like a real SAR ADC rather than a perfect number.
 */
#include <pthread.h>
#include <stdlib.h>

#include "esp_adc/adc_oneshot.h"
#include "sim_hw.h"

#define NUNITS 2
#define NCH 10

struct adc_oneshot_unit_ctx_t {
    adc_unit_t unit;
    int configured[NCH];
    adc_bitwidth_t bits[NCH];
};

static int s_raw[NUNITS][NCH];
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;

void sim_adc_set_raw(adc_unit_t unit, adc_channel_t ch, int raw)
{
    if (unit < 0 || unit >= NUNITS || ch < 0 || ch >= NCH) return;
    if (raw < 0) raw = 0;
    if (raw > 4095) raw = 4095;
    pthread_mutex_lock(&s_lock);
    s_raw[unit][ch] = raw;
    pthread_mutex_unlock(&s_lock);
}

int sim_adc_get_raw(adc_unit_t unit, adc_channel_t ch)
{
    if (unit < 0 || unit >= NUNITS || ch < 0 || ch >= NCH) return 0;
    pthread_mutex_lock(&s_lock);
    int v = s_raw[unit][ch];
    pthread_mutex_unlock(&s_lock);
    return v;
}

esp_err_t adc_oneshot_new_unit(const adc_oneshot_unit_init_cfg_t *init_config,
                               adc_oneshot_unit_handle_t *ret_unit)
{
    if (!init_config || !ret_unit || init_config->unit_id >= NUNITS) return ESP_ERR_INVALID_ARG;
    struct adc_oneshot_unit_ctx_t *u = calloc(1, sizeof(*u));
    if (!u) return ESP_ERR_NO_MEM;
    u->unit = init_config->unit_id;
    *ret_unit = u;
    return ESP_OK;
}

esp_err_t adc_oneshot_config_channel(adc_oneshot_unit_handle_t handle, adc_channel_t channel,
                                     const adc_oneshot_chan_cfg_t *config)
{
    if (!handle || !config || channel < 0 || channel >= NCH) return ESP_ERR_INVALID_ARG;
    handle->configured[channel] = 1;
    handle->bits[channel] = config->bitwidth == ADC_BITWIDTH_DEFAULT ? ADC_BITWIDTH_12 : config->bitwidth;
    return ESP_OK;
}

esp_err_t adc_oneshot_read(adc_oneshot_unit_handle_t handle, adc_channel_t chan, int *out_raw)
{
    if (!handle || !out_raw || chan < 0 || chan >= NCH) return ESP_ERR_INVALID_ARG;
    if (!handle->configured[chan]) return ESP_ERR_INVALID_STATE;
    int v = sim_adc_get_raw(handle->unit, chan) + (rand() % 7) - 3; /* +-3 LSB noise */
    if (v < 0) v = 0;
    if (v > 4095) v = 4095;
    *out_raw = v >> (12 - handle->bits[chan]);
    return ESP_OK;
}
