/* ESP32 mock platform - I2S in standard (Philips) mode, transmit only: the
 * subset of ESP-IDF's driver/i2s_std.h (and i2s_common.h) the firmware uses.
 * Names and fields as in ESP-IDF v6 for the ESP32-S3. The frames written
 * play out with the chip's clock and go to what the board wired to the
 * port (sim_i2s_set_sink in sim_hw.h). */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "driver/gpio.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define I2S_NUM_0    0
#define I2S_NUM_1    1
#define I2S_NUM_AUTO (-1)
#define I2S_GPIO_UNUSED GPIO_NUM_NC

typedef enum { I2S_ROLE_MASTER, I2S_ROLE_SLAVE } i2s_role_t;
typedef enum { I2S_DATA_BIT_WIDTH_8BIT = 8, I2S_DATA_BIT_WIDTH_16BIT = 16, I2S_DATA_BIT_WIDTH_24BIT = 24,
               I2S_DATA_BIT_WIDTH_32BIT = 32 } i2s_data_bit_width_t;
typedef enum { I2S_SLOT_BIT_WIDTH_AUTO = 0, I2S_SLOT_BIT_WIDTH_16BIT = 16, I2S_SLOT_BIT_WIDTH_32BIT = 32 } i2s_slot_bit_width_t;
typedef enum { I2S_SLOT_MODE_MONO = 1, I2S_SLOT_MODE_STEREO = 2 } i2s_slot_mode_t;
typedef enum { I2S_STD_SLOT_LEFT = 1, I2S_STD_SLOT_RIGHT = 2, I2S_STD_SLOT_BOTH = 3 } i2s_std_slot_mask_t;
typedef enum { I2S_CLK_SRC_DEFAULT = 0 } i2s_clock_src_t;
typedef enum { I2S_MCLK_MULTIPLE_128 = 128, I2S_MCLK_MULTIPLE_256 = 256, I2S_MCLK_MULTIPLE_384 = 384 } i2s_mclk_multiple_t;

typedef struct i2s_channel_obj_t *i2s_chan_handle_t;

typedef struct {
    int id;
    i2s_role_t role;
    uint32_t dma_desc_num;
    uint32_t dma_frame_num;
    union {
        bool auto_clear;
        bool auto_clear_after_cb;
    };
    bool auto_clear_before_cb;
    bool allow_pd;
    int intr_priority;
} i2s_chan_config_t;

#define I2S_CHANNEL_DEFAULT_CONFIG(i2s_num, i2s_role) { \
    .id = i2s_num, .role = i2s_role, .dma_desc_num = 6, .dma_frame_num = 240, \
    .auto_clear_after_cb = false, .auto_clear_before_cb = false, .allow_pd = false, .intr_priority = 0, }

typedef struct {
    i2s_data_bit_width_t data_bit_width;
    i2s_slot_bit_width_t slot_bit_width;
    i2s_slot_mode_t slot_mode;
    i2s_std_slot_mask_t slot_mask;
    uint32_t ws_width;
    bool ws_pol;
    bool bit_shift;
    bool left_align;
    bool big_endian;
    bool bit_order_lsb;
} i2s_std_slot_config_t;

typedef struct {
    uint32_t sample_rate_hz;
    i2s_clock_src_t clk_src;
    uint32_t ext_clk_freq_hz;
    i2s_mclk_multiple_t mclk_multiple;
    uint32_t bclk_div;
} i2s_std_clk_config_t;

typedef struct {
    gpio_num_t mclk, bclk, ws, dout, din;
    struct {
        uint32_t mclk_inv : 1;
        uint32_t bclk_inv : 1;
        uint32_t ws_inv : 1;
    } invert_flags;
} i2s_std_gpio_config_t;

typedef struct {
    i2s_std_clk_config_t clk_cfg;
    i2s_std_slot_config_t slot_cfg;
    i2s_std_gpio_config_t gpio_cfg;
} i2s_std_config_t;

#define I2S_STD_CLK_DEFAULT_CONFIG(rate) { \
    .sample_rate_hz = rate, .clk_src = I2S_CLK_SRC_DEFAULT, .ext_clk_freq_hz = 0, \
    .mclk_multiple = I2S_MCLK_MULTIPLE_256, .bclk_div = 8, }

#define I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(bits_per_sample, mono_or_stereo) { \
    .data_bit_width = bits_per_sample, .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO, .slot_mode = mono_or_stereo, \
    .slot_mask = I2S_STD_SLOT_BOTH, .ws_width = bits_per_sample, .ws_pol = false, .bit_shift = true, \
    .left_align = true, .big_endian = false, .bit_order_lsb = false, }

/** A transmit channel only (ret_rx_handle must be NULL): an RX channel isn't simulated. */
esp_err_t i2s_new_channel(const i2s_chan_config_t *chan_cfg, i2s_chan_handle_t *ret_tx_handle,
                          i2s_chan_handle_t *ret_rx_handle);
esp_err_t i2s_del_channel(i2s_chan_handle_t handle);
/** 16-bit samples only, mono or stereo. */
esp_err_t i2s_channel_init_std_mode(i2s_chan_handle_t handle, const i2s_std_config_t *std_cfg);
esp_err_t i2s_channel_enable(i2s_chan_handle_t handle);
/** Stops at once: what is still in the DMA buffers doesn't play. */
esp_err_t i2s_channel_disable(i2s_chan_handle_t handle);
/** Copies into the DMA buffers, waiting (up to timeout_ms) while they're full. */
esp_err_t i2s_channel_write(i2s_chan_handle_t handle, const void *src, size_t size, size_t *bytes_written,
                            uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
