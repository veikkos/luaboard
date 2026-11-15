/* ESP32 mock platform - subset of ESP-IDF <driver/gpio.h>. */
#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ESP32-S3 pad numbering: GPIO0..21 and GPIO26..48 (22..25 do not exist). */
typedef enum {
    GPIO_NUM_NC = -1,
    GPIO_NUM_0 = 0, GPIO_NUM_1, GPIO_NUM_2, GPIO_NUM_3, GPIO_NUM_4, GPIO_NUM_5,
    GPIO_NUM_6, GPIO_NUM_7, GPIO_NUM_8, GPIO_NUM_9, GPIO_NUM_10, GPIO_NUM_11,
    GPIO_NUM_12, GPIO_NUM_13, GPIO_NUM_14, GPIO_NUM_15, GPIO_NUM_16, GPIO_NUM_17,
    GPIO_NUM_18, GPIO_NUM_19, GPIO_NUM_20, GPIO_NUM_21,
    GPIO_NUM_26 = 26, GPIO_NUM_27, GPIO_NUM_28, GPIO_NUM_29, GPIO_NUM_30, GPIO_NUM_31,
    GPIO_NUM_32, GPIO_NUM_33, GPIO_NUM_34, GPIO_NUM_35, GPIO_NUM_36, GPIO_NUM_37,
    GPIO_NUM_38, GPIO_NUM_39, GPIO_NUM_40, GPIO_NUM_41, GPIO_NUM_42, GPIO_NUM_43,
    GPIO_NUM_44, GPIO_NUM_45, GPIO_NUM_46, GPIO_NUM_47, GPIO_NUM_48,
    GPIO_NUM_MAX,
} gpio_num_t;

#define GPIO_IS_VALID_GPIO(n) ((n) >= 0 && (n) < GPIO_NUM_MAX && !((n) >= 22 && (n) <= 25))

typedef enum {
    GPIO_MODE_DISABLE = 0,
    GPIO_MODE_INPUT = 1,
    GPIO_MODE_OUTPUT = 2,
    GPIO_MODE_OUTPUT_OD = 6,
    GPIO_MODE_INPUT_OUTPUT_OD = 7,
    GPIO_MODE_INPUT_OUTPUT = 3,
} gpio_mode_t;

typedef enum { GPIO_PULLUP_DISABLE = 0, GPIO_PULLUP_ENABLE = 1 } gpio_pullup_t;
typedef enum { GPIO_PULLDOWN_DISABLE = 0, GPIO_PULLDOWN_ENABLE = 1 } gpio_pulldown_t;
typedef enum { GPIO_INTR_DISABLE = 0 } gpio_int_type_t;

typedef enum {
    GPIO_PULLUP_ONLY,
    GPIO_PULLDOWN_ONLY,
    GPIO_PULLUP_PULLDOWN,
    GPIO_FLOATING,
} gpio_pull_mode_t;

typedef struct {
    uint64_t pin_bit_mask;
    gpio_mode_t mode;
    gpio_pullup_t pull_up_en;
    gpio_pulldown_t pull_down_en;
    gpio_int_type_t intr_type;
} gpio_config_t;

esp_err_t gpio_config(const gpio_config_t *cfg);
esp_err_t gpio_reset_pin(gpio_num_t gpio_num);
esp_err_t gpio_set_direction(gpio_num_t gpio_num, gpio_mode_t mode);
esp_err_t gpio_set_pull_mode(gpio_num_t gpio_num, gpio_pull_mode_t pull);
esp_err_t gpio_set_level(gpio_num_t gpio_num, uint32_t level);
int gpio_get_level(gpio_num_t gpio_num);

#ifdef __cplusplus
}
#endif
