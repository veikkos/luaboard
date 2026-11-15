/*
 * ESP32 mock platform - UART.
 *
 * A UART is backed by the host's serial link (host.h: a pty, or a named pipe
 * on Windows), which plays the role of a USB-UART bridge. If nobody is
 * listening and the link's buffer is full, bytes are dropped, like a real
 * UART TX line with nothing attached.
 */
#include "driver/uart.h"

#include <pthread.h>
#include <stdlib.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/task.h"
#include "mock_internal.h"
#include "sim_hw.h"

static const char *TAG = "serial";

/* A port with its TX wired to its RX (sim_uart_loopback): what's written
   comes back, up to this much unread. */
#define LOOP_CAP 4096

typedef struct {
    int installed;
    host_link_t *link;
    int baud;
    bool loop;
    uint8_t *fifo;               /* [LOOP_CAP], allocated on first use */
    size_t head, len;
    int tx, rx;                  /* the pins uart_set_pin gave it, -1 if none */
} uart_state_t;

static pthread_mutex_t s_loop_mu = PTHREAD_MUTEX_INITIALIZER;

static uart_state_t s_uart[UART_NUM_MAX] = {
    { .baud = 115200, .tx = -1, .rx = -1 }, { .baud = 115200, .tx = -1, .rx = -1 },
    { .baud = 115200, .tx = -1, .rx = -1 },
};

void sim_uart_loopback(uart_port_t port, bool on)
{
    if (port < 0 || port >= UART_NUM_MAX) return;
    pthread_mutex_lock(&s_loop_mu);
    s_uart[port].loop = on;
    s_uart[port].head = s_uart[port].len = 0;
    pthread_mutex_unlock(&s_loop_mu);
}

static int loop_write(uart_state_t *u, const uint8_t *src, size_t n)
{
    pthread_mutex_lock(&s_loop_mu);
    if (!u->fifo) u->fifo = malloc(LOOP_CAP);
    size_t room = u->fifo ? LOOP_CAP - u->len : 0, take = n < room ? n : room;   /* the rest is lost, as on a wire */
    for (size_t i = 0; i < take; i++) u->fifo[(u->head + u->len + i) % LOOP_CAP] = src[i];
    u->len += take;
    pthread_mutex_unlock(&s_loop_mu);
    return (int)n;
}

static int loop_read(uart_state_t *u, uint8_t *buf, size_t n)
{
    pthread_mutex_lock(&s_loop_mu);
    size_t take = n < u->len ? n : u->len;
    for (size_t i = 0; i < take; i++) buf[i] = u->fifo[(u->head + i) % LOOP_CAP];
    u->head = (u->head + take) % LOOP_CAP;
    u->len -= take;
    pthread_mutex_unlock(&s_loop_mu);
    return (int)take;
}

void sim_uart_attach_link(uart_port_t port, host_link_t *link)
{
    if (port < 0 || port >= UART_NUM_MAX) return;
    s_uart[port].link = link;
}

esp_err_t uart_driver_install(uart_port_t port, int rx_buffer_size, int tx_buffer_size,
                              int queue_size, QueueHandle_t *uart_queue, int intr_alloc_flags)
{
    (void)tx_buffer_size; (void)queue_size; (void)intr_alloc_flags;
    if (port < 0 || port >= UART_NUM_MAX || rx_buffer_size <= 128) return ESP_ERR_INVALID_ARG;
    if (uart_queue) *uart_queue = NULL;
    s_uart[port].installed = 1;
    return ESP_OK;
}

esp_err_t uart_param_config(uart_port_t port, const uart_config_t *cfg)
{
    if (port < 0 || port >= UART_NUM_MAX || !cfg) return ESP_ERR_INVALID_ARG;
    s_uart[port].baud = cfg->baud_rate;
    return ESP_OK;
}

esp_err_t uart_set_pin(uart_port_t port, int tx, int rx, int rts, int cts)
{
    (void)rts; (void)cts;
    if (port < 0 || port >= UART_NUM_MAX) return ESP_ERR_INVALID_ARG;
    /* As ESP-IDF 6 does: TX becomes an output and RX an input, their pulls
       as they were. */
    if (tx >= 0) {
        gpio_set_direction((gpio_num_t)tx, GPIO_MODE_OUTPUT);
        s_uart[port].tx = tx;
    }
    if (rx >= 0) {
        gpio_set_direction((gpio_num_t)rx, GPIO_MODE_INPUT);
        s_uart[port].rx = rx;
    }
    return ESP_OK;
}

/* Shared by the UART and USB-Serial/JTAG mocks. */
int mock_link_read(host_link_t *link, void *buf, uint32_t length, TickType_t ticks_to_wait)
{
    if (!link) {
        if (ticks_to_wait) vTaskDelay(ticks_to_wait);
        return 0;
    }
    int timeout_ms = ticks_to_wait == portMAX_DELAY ? -1 : (int)(ticks_to_wait * portTICK_PERIOD_MS);
    if (timeout_ms == 0 && !sim_link_look()) return 0;
    int n = host_link_read(link, buf, length, timeout_ms);
    if (n == HOST_LINK_HUNG_UP) {
        /* Nobody has the other side open: behave like an idle line. */
        if (timeout_ms > 0) vTaskDelay(ticks_to_wait);
        return 0;
    }
    return n;
}

int mock_link_write(host_link_t *link, const void *src, size_t size, int stall_timeout_ms)
{
    if (!link) return (int)size; /* nothing attached: bytes vanish on the wire */
    /* Like a USB CDC endpoint: wait while the host drains the buffer, but drop
       the data if no progress is made for stall_timeout_ms (no host reading).
       After a drop, further writes drop immediately until the buffer drains,
       so a script printing with no PC attached doesn't stall the main loop. */
    static host_link_t *s_stalled;
    int n = host_link_write(link, src, size, s_stalled == link ? 0 : stall_timeout_ms);
    if (n == (int)size) {
        s_stalled = NULL;
    } else {
        if (s_stalled != link) ESP_LOGW(TAG, "TX stalled, dropping output until the host reads");
        s_stalled = link;
    }
    return n;
}

int uart_read_bytes(uart_port_t port, void *buf, uint32_t length, TickType_t ticks_to_wait)
{
    if (port < 0 || port >= UART_NUM_MAX || !s_uart[port].installed) return -1;
    if (s_uart[port].loop) {
        int n = loop_read(&s_uart[port], buf, length);
        if (!n && ticks_to_wait) vTaskDelay(ticks_to_wait);
        return n;
    }
    return mock_link_read(s_uart[port].link, buf, length, ticks_to_wait);
}

int uart_write_bytes(uart_port_t port, const void *src, size_t size)
{
    if (port < 0 || port >= UART_NUM_MAX || !s_uart[port].installed) return -1;
    if (s_uart[port].loop) return loop_write(&s_uart[port], src, size);
    return mock_link_write(s_uart[port].link, src, size, 50);
}

esp_err_t uart_driver_delete(uart_port_t port)
{
    if (port < 0 || port >= UART_NUM_MAX) return ESP_ERR_INVALID_ARG;
    s_uart[port].installed = 0;
    /* As ESP-IDF 6 does: TX is no longer driven; RX stays an input until
       someone resets the pin. */
    if (s_uart[port].tx >= 0) gpio_set_direction((gpio_num_t)s_uart[port].tx, GPIO_MODE_DISABLE);
    s_uart[port].tx = s_uart[port].rx = -1;
    pthread_mutex_lock(&s_loop_mu);
    s_uart[port].head = s_uart[port].len = 0;        /* what wasn't read is gone with the driver */
    pthread_mutex_unlock(&s_loop_mu);
    return ESP_OK;
}

bool uart_is_driver_installed(uart_port_t port)
{
    return port >= 0 && port < UART_NUM_MAX && s_uart[port].installed;
}

esp_err_t uart_get_tx_buffer_free_size(uart_port_t port, size_t *size)
{
    if (port < 0 || port >= UART_NUM_MAX || !size) return ESP_ERR_INVALID_ARG;
    *size = 2048;                                    /* the wire takes it as fast as it's written */
    return ESP_OK;
}

esp_err_t uart_wait_tx_done(uart_port_t port, TickType_t ticks_to_wait)
{
    (void)ticks_to_wait;
    if (port < 0 || port >= UART_NUM_MAX || !s_uart[port].installed) return ESP_FAIL;
    return ESP_OK;                                   /* written is sent: the wire takes it at once */
}
