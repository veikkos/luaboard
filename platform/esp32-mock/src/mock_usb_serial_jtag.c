/*
 * ESP32 mock platform - USB-Serial/JTAG (ESP32-S3 built-in USB CDC).
 * Backed by the host's serial link, like the UART mock; on the PC it shows
 * up as the same kind of serial port (/dev/cu.usbmodem* or COMn on real
 * hardware).
 */
#include <stdarg.h>
#include <stdio.h>
#include "esp_rom_sys.h"
#include "driver/usb_serial_jtag.h"
#include "mock_internal.h"
#include "sim_hw.h"

#include <stdlib.h>
#include <string.h>

#include "freertos/task.h"
#include "hal/usb_serial_jtag_ll.h"

#define FIFO_SIZE 64   /* the endpoint's: one packet */

static host_link_t *s_link;
static int s_installed;
static bool s_host_gone, s_pulled_off;   /* the computer has given up on the port (see usb_serial_jtag_is_connected) */
static uint32_t s_tx_size;

/* Receiving, as on the chip. A packet from the PC lands in the 64-byte FIFO,
   and until that is read the chip takes no other: the PC waits (here: the
   bytes stay in the link). The driver's receive interrupt, on from
   usb_serial_jtag_driver_install, empties the FIFO into the driver's ring
   buffer the moment a packet is in, and a packet the ring has no room for is
   dropped: the driver doesn't look at what the ring said. So with the
   interrupt on, whatever the firmware hasn't read in time beyond the ring's
   size is lost. Firmware that turns the interrupt off reads the FIFO itself
   (usb_serial_jtag_ll_read_rxfifo) and loses nothing. */
static uint8_t s_fifo[FIFO_SIZE];
static uint32_t s_fifo_len, s_fifo_off;
static uint8_t *s_ring;
static uint32_t s_ring_size, s_ring_len;
static bool s_rx_intr;

/* The next packet from the PC into the empty FIFO. */
static void fifo_fill(void)
{
    if (s_fifo_off < s_fifo_len || !s_link) return;
    s_fifo_off = s_fifo_len = 0;
    int n = mock_link_read(s_link, s_fifo, FIFO_SIZE, 0);
    if (s_host_gone) n = 0;                          /* (what it writes meanwhile never leaves it) */
    if (n > 0) s_fifo_len = (uint32_t)n;
}

/* The receive interrupt, for every packet that has come by now. */
static void rx_isr(void)
{
    while (s_rx_intr) {
        fifo_fill();
        uint32_t n = s_fifo_len - s_fifo_off;
        if (!n) return;
        if (n <= s_ring_size - s_ring_len) {
            memcpy(s_ring + s_ring_len, s_fifo + s_fifo_off, n);
            s_ring_len += n;
        }
        s_fifo_off = s_fifo_len;
    }
}

void usb_serial_jtag_ll_ena_intr_mask(uint32_t mask)
{
    if (mask & USB_SERIAL_JTAG_INTR_SERIAL_OUT_RECV_PKT) s_rx_intr = true;
}

void usb_serial_jtag_ll_disable_intr_mask(uint32_t mask)
{
    if (mask & USB_SERIAL_JTAG_INTR_SERIAL_OUT_RECV_PKT) s_rx_intr = false;
}

uint32_t usb_serial_jtag_ll_read_rxfifo(uint8_t *buf, uint32_t rd_len)
{
    rx_isr();   /* (with its interrupt on, the driver has the bytes first) */
    fifo_fill();
    uint32_t n = s_fifo_len - s_fifo_off;
    if (n > rd_len) n = rd_len;
    memcpy(buf, s_fifo + s_fifo_off, n);
    s_fifo_off += n;
    return n;
}

void sim_usb_serial_attach_link(host_link_t *link) { s_link = link; }

esp_err_t usb_serial_jtag_driver_install(usb_serial_jtag_driver_config_t *cfg)
{
    if (!cfg || cfg->rx_buffer_size == 0 || cfg->tx_buffer_size == 0) return ESP_ERR_INVALID_ARG;
    free(s_ring);
    s_ring = malloc(cfg->rx_buffer_size);
    if (!s_ring) return ESP_ERR_NO_MEM;
    s_ring_size = cfg->rx_buffer_size;
    s_ring_len = 0;
    s_rx_intr = true;
    s_installed = 1;
    s_tx_size = cfg->tx_buffer_size;
    return ESP_OK;
}

int usb_serial_jtag_read_bytes(void *buf, uint32_t length, TickType_t ticks_to_wait)
{
    if (!s_installed) return -1;
    rx_isr();
    if (!s_ring_len && ticks_to_wait) {
        vTaskDelay(ticks_to_wait);
        rx_isr();
    }
    uint32_t n = s_ring_len < length ? s_ring_len : length;
    memcpy(buf, s_ring, n);
    memmove(s_ring, s_ring + n, s_ring_len - n);
    s_ring_len -= n;
    return (int)n;
}

int usb_serial_jtag_write_bytes(const void *src, size_t size, TickType_t ticks_to_wait)
{
    if (!s_installed) return -1;
    /* Like the real driver: a write is queued into the TX ring buffer whole,
       so one bigger than the buffer fails and nothing is sent. */
    if (size > s_tx_size || s_host_gone) return 0;
    int ms = ticks_to_wait == portMAX_DELAY ? -1 : (int)(ticks_to_wait * portTICK_PERIOD_MS);
    return mock_link_write(s_link, src, size, ms);
}

/* The ROM's printf writes to this port too, while its g_usb_print is set.
   What it writes lands in the middle of whatever the firmware is sending. */
bool g_usb_print = true;

int esp_rom_printf(const char *fmt, ...)
{
    char line[160];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (n < 0) return n;
    if ((size_t)n >= sizeof line) n = sizeof line - 1;
    fputs(line, stderr);
    if (g_usb_print && s_link && !s_host_gone) {
        fprintf(stderr, "[sim] the ROM's printf wrote onto the USB link: %s", line);
        mock_link_write(s_link, line, (size_t)n, 0);
    }
    return n;
}

/* The computer can give up on the port while the chip restarts (seen on a
   real board: the port stays listed there, but nothing more comes or goes,
   not even the frames a connected port sees every millisecond). It starts
   on the device anew when D+'s pull-up drops and comes back, as at a plug
   pulled and put back. */

void sim_usb_serial_host_gone(bool gone) { s_host_gone = gone; }
bool sim_usb_serial_host_talking(void) { return !s_host_gone; }

void usb_serial_jtag_ll_phy_enable_pull_override(const usb_serial_jtag_pull_override_vals_t *vals)
{
    if (!vals->dp_pu) s_pulled_off = true;
    else if (s_pulled_off) s_pulled_off = s_host_gone = false;
}

void usb_serial_jtag_ll_phy_disable_pull_override(void) { }

bool usb_serial_jtag_is_connected(void) { return s_installed && s_link && !s_host_gone; }
