/*
 * Behavioural model of an ILI9342C 320x240 TFT controller on SPI.
 *
 * Decodes the byte stream the firmware's driver sends, using the D/C pin
 * level (read from the GPIO mock) to tell commands from parameters/pixels:
 * SLPOUT/SLPIN, DISPON/OFF, INVON/OFF, MADCTL (BGR), COLMOD, CASET/RASET
 * windows and RAMWR pixel streams (RGB565, big-endian on the wire).
 */
#pragma once

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    pthread_mutex_t lock;
    int w, h;
    int dc_pin;
    uint16_t *gram;             /* w*h, as written (RGB565) */
    bool sleeping, display_on, inverted, bgr;
    int colmod;
    int x0, x1, y0, y1, cx, cy; /* address window and write cursor */
    uint8_t cmd, params[8];
    int nparams;
    int pix_hi;                 /* first byte of a pixel pair, -1 if none */
    uint32_t frames;            /* bumps on every RAMWR, for change detection */
} ili9342c_t;

void ili9342c_init(ili9342c_t *m, int w, int h, int dc_pin);
/** Hardware reset (RESX low pulse). */
void ili9342c_reset(ili9342c_t *m);
/** sim_spi_write_fn compatible. */
void ili9342c_spi_write(void *ctx, const uint8_t *data, size_t len);
/** What a viewer sees, as 8-bit RGB triplets (w*h*3). Off/asleep = black. */
void ili9342c_view_rgb(ili9342c_t *m, uint8_t *rgb);
