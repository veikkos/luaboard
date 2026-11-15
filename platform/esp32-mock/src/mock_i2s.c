/*
 * ESP32 mock platform - I2S transmit channels (driver/i2s_std.h).
 *
 * A channel's DMA buffers are a ring of dma_desc_num * dma_frame_num frames.
 * The frames play out with the chip's clock at the sample rate: whenever
 * the firmware writes, the frames whose time has come go to the port's sink
 * (sim_i2s_set_sink), and a write waits while the ring is full, as the
 * driver does on the chip. A ring that ran empty starts again when the next
 * frame is written (the chip sends silence meanwhile). Disabling a channel
 * drops what hasn't played yet, as on the chip.
 */
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "driver/i2s_std.h"
#include "esp_timer.h"
#include "mock_internal.h"
#include "sim_hw.h"

#define PORTS 2

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static struct {
    sim_i2s_sink_fn fn;
    void *ctx;
} s_sink[PORTS];
static bool s_used[PORTS];

struct i2s_channel_obj_t {
    int port;
    bool enabled;
    uint32_t rate;
    int channels;              /* 0 until init_std_mode */
    size_t cap;                /* frames in the DMA buffers */
    int16_t *ring;             /* cap * channels samples */
    uint64_t head, tail;       /* frames played so far / written so far */
    int64_t base_us;           /* frame base_frame plays at this chip time */
    uint64_t base_frame;
};

void sim_i2s_set_sink(int port, sim_i2s_sink_fn fn, void *ctx)
{
    if (port < 0 || port >= PORTS) return;
    pthread_mutex_lock(&s_lock);
    s_sink[port].fn = fn;
    s_sink[port].ctx = ctx;
    pthread_mutex_unlock(&s_lock);
}

/* The chip time frame `f` plays at (rounded up). */
static int64_t frame_us(const struct i2s_channel_obj_t *c, uint64_t f)
{
    return c->base_us + (int64_t)(((f - c->base_frame) * 1000000ULL + c->rate - 1) / c->rate);
}

/* Hand the frames whose time has come to the sink. */
static void play_due(struct i2s_channel_obj_t *c, int64_t now)
{
    if (c->head == c->tail || now < c->base_us) return;
    uint64_t due = c->base_frame + (uint64_t)((now - c->base_us) * (int64_t)c->rate / 1000000);
    if (due > c->tail) due = c->tail;
    pthread_mutex_lock(&s_lock);
    sim_i2s_sink_fn fn = s_sink[c->port].fn;
    void *ctx = s_sink[c->port].ctx;
    pthread_mutex_unlock(&s_lock);
    while (c->head < due) {
        size_t at = (size_t)(c->head % c->cap), n = (size_t)(due - c->head);
        if (n > c->cap - at) n = c->cap - at;                 /* up to the ring's end */
        if (fn) fn(ctx, c->ring + at * (size_t)c->channels, n, c->channels, c->rate);
        c->head += n;
    }
}

esp_err_t i2s_new_channel(const i2s_chan_config_t *cfg, i2s_chan_handle_t *tx, i2s_chan_handle_t *rx)
{
    if (!cfg || !tx || cfg->role != I2S_ROLE_MASTER || cfg->dma_desc_num < 2 || cfg->dma_frame_num == 0)
        return ESP_ERR_INVALID_ARG;
    if (rx) return ESP_ERR_NOT_SUPPORTED;                     /* only transmit is simulated */
    pthread_mutex_lock(&s_lock);
    int port = cfg->id;
    if (port == I2S_NUM_AUTO)
        for (port = 0; port < PORTS && s_used[port]; port++) {}
    bool ok = port >= 0 && port < PORTS && !s_used[port];
    if (ok) s_used[port] = true;
    pthread_mutex_unlock(&s_lock);
    if (!ok) return ESP_ERR_NOT_FOUND;
    struct i2s_channel_obj_t *c = calloc(1, sizeof *c);
    if (!c) return ESP_ERR_NO_MEM;
    c->port = port;
    c->cap = (size_t)cfg->dma_desc_num * cfg->dma_frame_num;
    *tx = c;
    return ESP_OK;
}

esp_err_t i2s_channel_init_std_mode(i2s_chan_handle_t c, const i2s_std_config_t *cfg)
{
    if (!c || !cfg || c->enabled) return ESP_ERR_INVALID_STATE;
    if (cfg->slot_cfg.data_bit_width != I2S_DATA_BIT_WIDTH_16BIT) return ESP_ERR_NOT_SUPPORTED;
    if (cfg->clk_cfg.sample_rate_hz < 1000 || cfg->clk_cfg.sample_rate_hz > 192000) return ESP_ERR_INVALID_ARG;
    c->rate = cfg->clk_cfg.sample_rate_hz;
    c->channels = cfg->slot_cfg.slot_mode == I2S_SLOT_MODE_MONO ? 1 : 2;
    free(c->ring);
    c->ring = calloc(c->cap * (size_t)c->channels, sizeof *c->ring);
    return c->ring ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t i2s_channel_enable(i2s_chan_handle_t c)
{
    if (!c || !c->channels || c->enabled) return ESP_ERR_INVALID_STATE;
    c->enabled = true;
    c->head = c->tail = 0;
    return ESP_OK;
}

esp_err_t i2s_channel_disable(i2s_chan_handle_t c)
{
    if (!c || !c->enabled) return ESP_ERR_INVALID_STATE;
    play_due(c, esp_timer_get_time());
    c->head = c->tail;                                        /* the rest is dropped */
    c->enabled = false;
    return ESP_OK;
}

esp_err_t i2s_del_channel(i2s_chan_handle_t c)
{
    if (!c || c->enabled) return ESP_ERR_INVALID_STATE;
    pthread_mutex_lock(&s_lock);
    s_used[c->port] = false;
    pthread_mutex_unlock(&s_lock);
    free(c->ring);
    free(c);
    return ESP_OK;
}

esp_err_t i2s_channel_write(i2s_chan_handle_t c, const void *src, size_t size, size_t *written, uint32_t timeout_ms)
{
    if (written) *written = 0;
    if (!c || !src) return ESP_ERR_INVALID_ARG;
    if (!c->enabled) return ESP_ERR_INVALID_STATE;
    size_t fbytes = sizeof(int16_t) * (size_t)c->channels;
    const int16_t *in = src;
    uint64_t left = size / fbytes;
    int64_t deadline = timeout_ms == UINT32_MAX ? INT64_MAX : esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    while (left) {
        int64_t now = esp_timer_get_time();
        play_due(c, now);
        if (c->head == c->tail) {                             /* ran empty: the next frame plays now */
            c->base_us = now;
            c->base_frame = c->tail;
        }
        size_t space = c->cap - (size_t)(c->tail - c->head);
        if (!space) {                                         /* full: wait until a buffer's worth has played */
            uint64_t need = left < c->cap / 2 ? left : c->cap / 2;
            int64_t at = frame_us(c, c->head + need);
            if (at > deadline) {
                mock_sleep_until_us(deadline);
                play_due(c, esp_timer_get_time());
                if (c->tail - c->head == c->cap) return ESP_ERR_TIMEOUT;
                continue;
            }
            mock_sleep_until_us(at);
            continue;
        }
        size_t n = left < space ? (size_t)left : space;
        for (size_t done = 0; done < n;) {                    /* copy, wrapping around the ring */
            size_t at = (size_t)(c->tail % c->cap), k = n - done;
            if (k > c->cap - at) k = c->cap - at;
            memcpy(c->ring + at * (size_t)c->channels, in, k * fbytes);
            in += k * (size_t)c->channels;
            c->tail += k;
            done += k;
        }
        left -= n;
        if (written) *written += n * fbytes;
    }
    return ESP_OK;
}
