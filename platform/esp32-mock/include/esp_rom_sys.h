/* esp_rom_sys.h stand-in: the busy wait of a few microseconds (nothing here
   needs the time to pass), and the ROM's printf with its output channels. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

static inline void esp_rom_delay_us(uint32_t us) { (void)us; }

/** The ROM's printf, for code that can't use the log calls (an interrupt).
 *  On the chip it writes to UART0 and, while the ROM's g_usb_print is set
 *  (it is, at start-up), to the USB port as well: here to stderr, and onto
 *  the simulated USB link. */
int esp_rom_printf(const char *fmt, ...);
extern bool g_usb_print;
