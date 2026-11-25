/*
 * Behavioural model of an ILI9342 320x240 TFT controller on SPI: the
 * ILI9342C, or the ILI9342E of boards made from August 2026.
 *
 * Decodes the byte stream the firmware's driver sends, using the D/C pin
 * level (read from the GPIO mock) to tell commands from parameters/pixels:
 * SLPOUT/SLPIN, DISPON/OFF, INVON/OFF, MADCTL (BGR), COLMOD, CASET/RASET
 * windows and RAMWR pixel streams (RGB565, big-endian on the wire).
 *
 * The two differ in what sets the panel up (its voltages and gamma curves),
 * which each needs before its picture is right:
 *   C: 0xC8 = FF 93 42 unlocks the extended commands, gamma in 0xE0 and 0xE1
 *   E: 0xDD = 01 unlocks them, gamma in 0xE4 and 0xE5 (0xC8 is a voltage)
 * Each takes 15 gamma values, and only while unlocked.
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
    char variant;               /* 'C' or 'E' */
    bool sleeping, display_on, inverted, bgr;
    bool unlocked;              /* the extended commands, by this variant's key */
    uint8_t gamma;              /* bit 0: the positive curve came, bit 1: the negative */
    bool warned;
    int colmod;
    int x0, x1, y0, y1, cx, cy; /* address window and write cursor */
    uint8_t cmd, params[16];
    int nparams;
    int pix_hi;                 /* first byte of a pixel pair, -1 if none */
    uint32_t frames;            /* bumps on every RAMWR, for change detection */
} ili9342_t;

void ili9342_init(ili9342_t *m, int w, int h, int dc_pin);
/** Which controller it is: 'C' (as from ili9342_init) or 'E'. Another panel:
 *  not set up. */
void ili9342_set_variant(ili9342_t *m, char variant);
/** Hardware reset (RESX low pulse). */
void ili9342_reset(ili9342_t *m);
/** sim_spi_write_fn compatible. */
void ili9342_spi_write(void *ctx, const uint8_t *data, size_t len);
/** What a viewer sees, as 8-bit RGB triplets (w*h*3). Off/asleep = black,
 *  and washed out unless the panel was set up (a stand-in for a picture
 *  that's off: how it really looks then hasn't been measured). */
void ili9342_view_rgb(ili9342_t *m, uint8_t *rgb);
