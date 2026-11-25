/* esp_rom_sys.h stand-in: the busy wait of a few microseconds (nothing here
   needs the time to pass). */
#pragma once

#include <stdint.h>

static inline void esp_rom_delay_us(uint32_t us) { (void)us; }
