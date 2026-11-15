/* ESP32 mock platform - subset of ESP-IDF <driver/ledc.h>: the PWM unit
   (4 timers, 8 channels on the ESP32-S3). Nothing pulses; the simulator
   can ask what a pin would put out (sim_pwm_get in sim_hw.h). */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { LEDC_LOW_SPEED_MODE = 0, LEDC_SPEED_MODE_MAX } ledc_mode_t;
typedef enum { LEDC_TIMER_0 = 0, LEDC_TIMER_1, LEDC_TIMER_2, LEDC_TIMER_3, LEDC_TIMER_MAX } ledc_timer_t;
typedef enum {
    LEDC_CHANNEL_0 = 0, LEDC_CHANNEL_1, LEDC_CHANNEL_2, LEDC_CHANNEL_3, LEDC_CHANNEL_4, LEDC_CHANNEL_5,
    LEDC_CHANNEL_6, LEDC_CHANNEL_7, LEDC_CHANNEL_MAX,
} ledc_channel_t;
typedef enum { LEDC_TIMER_1_BIT = 1, LEDC_TIMER_14_BIT = 14, LEDC_TIMER_BIT_MAX } ledc_timer_bit_t;   /* the value is the bits */
typedef enum { LEDC_AUTO_CLK = 0, LEDC_USE_APB_CLK } ledc_clk_cfg_t;

typedef struct {
    ledc_mode_t speed_mode;
    ledc_timer_bit_t duty_resolution;
    ledc_timer_t timer_num;
    uint32_t freq_hz;
    ledc_clk_cfg_t clk_cfg;
} ledc_timer_config_t;

typedef struct {
    int gpio_num;
    ledc_mode_t speed_mode;
    ledc_channel_t channel;
    ledc_timer_t timer_sel;
    uint32_t duty;               /* 0 .. 2^duty_resolution */
    int hpoint;
} ledc_channel_config_t;

/** The most bits of duty resolution a timer at timer_freq can have from a
 *  clock of src_clk_freq; 0 if that frequency can't be made. */
uint32_t ledc_find_suitable_duty_resolution(uint32_t src_clk_freq, uint32_t timer_freq);
esp_err_t ledc_timer_config(const ledc_timer_config_t *timer_conf);
esp_err_t ledc_channel_config(const ledc_channel_config_t *ledc_conf);
esp_err_t ledc_set_duty(ledc_mode_t speed_mode, ledc_channel_t channel, uint32_t duty);
esp_err_t ledc_update_duty(ledc_mode_t speed_mode, ledc_channel_t channel);
esp_err_t ledc_stop(ledc_mode_t speed_mode, ledc_channel_t channel, uint32_t idle_level);

#ifdef __cplusplus
}
#endif
