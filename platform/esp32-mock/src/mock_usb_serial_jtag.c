/*
 * ESP32 mock platform - USB-Serial/JTAG (ESP32-S3 built-in USB CDC).
 * Backed by the host's serial link, like the UART mock; on the PC it shows
 * up as the same kind of serial port (/dev/cu.usbmodem* or COMn on real
 * hardware).
 */
#include "driver/usb_serial_jtag.h"
#include "mock_internal.h"
#include "sim_hw.h"

static host_link_t *s_link;
static int s_installed;
static uint32_t s_tx_size;

void sim_usb_serial_attach_link(host_link_t *link) { s_link = link; }

esp_err_t usb_serial_jtag_driver_install(usb_serial_jtag_driver_config_t *cfg)
{
    if (!cfg || cfg->rx_buffer_size == 0 || cfg->tx_buffer_size == 0) return ESP_ERR_INVALID_ARG;
    s_installed = 1;
    s_tx_size = cfg->tx_buffer_size;
    return ESP_OK;
}

int usb_serial_jtag_read_bytes(void *buf, uint32_t length, TickType_t ticks_to_wait)
{
    if (!s_installed) return -1;
    return mock_link_read(s_link, buf, length, ticks_to_wait);
}

int usb_serial_jtag_write_bytes(const void *src, size_t size, TickType_t ticks_to_wait)
{
    if (!s_installed) return -1;
    /* Like the real driver: a write is queued into the TX ring buffer whole,
       so one bigger than the buffer fails and nothing is sent. */
    if (size > s_tx_size) return 0;
    int ms = ticks_to_wait == portMAX_DELAY ? -1 : (int)(ticks_to_wait * portTICK_PERIOD_MS);
    return mock_link_write(s_link, src, size, ms);
}

bool usb_serial_jtag_is_connected(void) { return s_installed && s_link; }
