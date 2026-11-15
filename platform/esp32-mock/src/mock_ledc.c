/* ESP32 mock platform - the PWM unit (driver/ledc.h): see the header. */
#include "driver/ledc.h"

#include <pthread.h>

#include "driver/gpio.h"
#include "sim_hw.h"

#define DIV_MAX 1024             /* the clock divider's whole part: 10 bits */

static pthread_mutex_t s_mu = PTHREAD_MUTEX_INITIALIZER;
static struct { uint32_t hz; int bits; } s_timer[LEDC_TIMER_MAX];
static struct { bool on; int pin; int timer; uint32_t duty, pending; } s_chan[LEDC_CHANNEL_MAX];

uint32_t ledc_find_suitable_duty_resolution(uint32_t src_clk_freq, uint32_t timer_freq)
{
    if (!timer_freq) return 0;
    for (int bits = 14; bits >= 1; bits--) {
        uint64_t steps = (uint64_t)timer_freq << bits;
        if (steps <= src_clk_freq && src_clk_freq / steps < DIV_MAX) return (uint32_t)bits;
    }
    return 0;
}

esp_err_t ledc_timer_config(const ledc_timer_config_t *c)
{
    if (!c || c->speed_mode != LEDC_LOW_SPEED_MODE || c->timer_num >= LEDC_TIMER_MAX || !c->freq_hz ||
        c->duty_resolution < 1 || c->duty_resolution > 14)
        return ESP_ERR_INVALID_ARG;
    pthread_mutex_lock(&s_mu);
    s_timer[c->timer_num].hz = c->freq_hz;
    s_timer[c->timer_num].bits = (int)c->duty_resolution;
    pthread_mutex_unlock(&s_mu);
    return ESP_OK;
}

esp_err_t ledc_channel_config(const ledc_channel_config_t *c)
{
    if (!c || c->speed_mode != LEDC_LOW_SPEED_MODE || c->channel >= LEDC_CHANNEL_MAX || c->timer_sel >= LEDC_TIMER_MAX)
        return ESP_ERR_INVALID_ARG;
    pthread_mutex_lock(&s_mu);
    s_chan[c->channel].on = true;
    s_chan[c->channel].pin = c->gpio_num;
    s_chan[c->channel].timer = (int)c->timer_sel;
    s_chan[c->channel].duty = s_chan[c->channel].pending = c->duty;
    pthread_mutex_unlock(&s_mu);
    /* As the chip's: the channel drives its pin from here on, also after
       ledc_stop (at the idle level), until someone resets the pin. */
    gpio_set_direction((gpio_num_t)c->gpio_num, GPIO_MODE_OUTPUT);
    return ESP_OK;
}

esp_err_t ledc_set_duty(ledc_mode_t mode, ledc_channel_t ch, uint32_t duty)
{
    if (mode != LEDC_LOW_SPEED_MODE || ch >= LEDC_CHANNEL_MAX) return ESP_ERR_INVALID_ARG;
    pthread_mutex_lock(&s_mu);
    s_chan[ch].pending = duty;               /* takes effect with ledc_update_duty, as on the chip */
    pthread_mutex_unlock(&s_mu);
    return ESP_OK;
}

esp_err_t ledc_update_duty(ledc_mode_t mode, ledc_channel_t ch)
{
    if (mode != LEDC_LOW_SPEED_MODE || ch >= LEDC_CHANNEL_MAX) return ESP_ERR_INVALID_ARG;
    pthread_mutex_lock(&s_mu);
    s_chan[ch].duty = s_chan[ch].pending;
    pthread_mutex_unlock(&s_mu);
    return ESP_OK;
}

esp_err_t ledc_stop(ledc_mode_t mode, ledc_channel_t ch, uint32_t idle_level)
{
    (void)idle_level;
    if (mode != LEDC_LOW_SPEED_MODE || ch >= LEDC_CHANNEL_MAX) return ESP_ERR_INVALID_ARG;
    pthread_mutex_lock(&s_mu);
    s_chan[ch].on = false;
    pthread_mutex_unlock(&s_mu);
    return ESP_OK;
}

bool sim_pwm_get(int pin, uint32_t *hz, double *duty)
{
    bool on = false;
    pthread_mutex_lock(&s_mu);
    for (int ch = 0; ch < LEDC_CHANNEL_MAX; ch++) {
        if (!s_chan[ch].on || s_chan[ch].pin != pin) continue;
        on = true;
        *hz = s_timer[s_chan[ch].timer].hz;
        int bits = s_timer[s_chan[ch].timer].bits;
        *duty = (double)s_chan[ch].duty / (double)(1u << bits);
        /* The chip's flaw: at its top resolution a duty of all the steps
           (100 %) isn't reachable, and comes out as nothing. */
        if (bits == 14 && s_chan[ch].duty >= (1u << bits)) *duty = 0;
    }
    pthread_mutex_unlock(&s_mu);
    return on;
}
