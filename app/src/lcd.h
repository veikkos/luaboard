/*
 * LCD panel driver interface. The panel is selected by the board: the
 * CoreS3 has an ILI9342C or ILI9342E 320x240 on SPI (lcd_ili9342.c).
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"

esp_err_t lcd_init(void);
/** Send the rectangle [x0..x1] x [y0..y1] (inclusive) of an RGB565
 *  framebuffer with row stride `stride` pixels to the panel. */
void lcd_flush(const uint16_t *fb, int stride, int x0, int y0, int x1, int y1);
