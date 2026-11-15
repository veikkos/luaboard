/*
 * Classic 5x7 bitmap font: printable ASCII 0x20..0x7E, and Latin-1
 * (U+00A0..U+00FF), Œ Š Ž Ÿ and € through font5x7_find().
 * Column-major: 5 bytes per glyph, bit 0 = top row.
 * Shared by the firmware (display text) and the simulator (panel labels).
 */
#pragma once

#include <stdint.h>

#define FONT5X7_FIRST  0x20
#define FONT5X7_LAST   0x7E
#define FONT5X7_W      5
#define FONT5X7_H      7

extern const uint8_t font5x7[(FONT5X7_LAST - FONT5X7_FIRST + 1) * FONT5X7_W];

/** Glyph columns for Unicode code point `cp`, or NULL if the font has none. */
const uint8_t *font5x7_find(uint32_t cp);

/** Glyph columns for an ASCII character; unknown chars map to '?'. */
static inline const uint8_t *font5x7_glyph(char c)
{
    unsigned char u = (unsigned char)c;
    if (u < FONT5X7_FIRST || u > FONT5X7_LAST) u = '?';
    return &font5x7[(u - FONT5X7_FIRST) * FONT5X7_W];
}
