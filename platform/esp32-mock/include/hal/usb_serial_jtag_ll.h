/* ESP32 mock platform - subset of ESP-IDF <hal/usb_serial_jtag_ll.h>: the
 * USB-Serial/JTAG's receive FIFO and its interrupt, below the driver
 * (mock_usb_serial_jtag.c says how the two behave). */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    USB_SERIAL_JTAG_INTR_SERIAL_OUT_RECV_PKT = (1 << 2),   /* a packet from the PC is in the FIFO */
} usb_serial_jtag_ll_intr_t;

void usb_serial_jtag_ll_ena_intr_mask(uint32_t mask);
void usb_serial_jtag_ll_disable_intr_mask(uint32_t mask);
/** Up to rd_len bytes of the packet in the FIFO; the count, 0 if it is empty. */
uint32_t usb_serial_jtag_ll_read_rxfifo(uint8_t *buf, uint32_t rd_len);

/* The pull resistors on the two data lines, set by hand: without the one
   on D+ the computer takes the device for unplugged, and with it back it
   starts on the device anew. */
typedef struct { bool dp_pu, dm_pu, dp_pd, dm_pd; } usb_serial_jtag_pull_override_vals_t;
void usb_serial_jtag_ll_phy_enable_pull_override(const usb_serial_jtag_pull_override_vals_t *vals);
void usb_serial_jtag_ll_phy_disable_pull_override(void);

#ifdef __cplusplus
}
#endif
