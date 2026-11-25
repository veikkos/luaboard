/*
 * 2D graphics on an RGB565 framebuffer sized by the board (BOARD_LCD_W x H).
 * All drawing clips to the screen; only the changed rectangle is sent to the
 * panel on gfx_flush().
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef uint16_t gfx_color_t;   /* RGB565 */

/* Coordinates and sizes are clamped to +-GFX_COORD_MAX: far off the screen,
   small enough that no drawing arithmetic overflows and every call is quick. */
#define GFX_COORD_MAX 32767

bool gfx_init(void);
int  gfx_width(void);
int  gfx_height(void);
const uint16_t *gfx_framebuffer(void);

static inline gfx_color_t gfx_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    return (gfx_color_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}
static inline gfx_color_t gfx_from_rgb888(uint32_t c)
{
    return gfx_rgb((uint8_t)(c >> 16), (uint8_t)(c >> 8), (uint8_t)c);
}
static inline uint32_t gfx_to_rgb888(gfx_color_t c)
{
    uint32_t r = (c >> 11) & 0x1F, g = (c >> 5) & 0x3F, b = c & 0x1F;
    return (((r * 527 + 23) >> 6) << 16) | (((g * 259 + 33) >> 6) << 8) | ((b * 527 + 23) >> 6);
}

void gfx_fill(gfx_color_t c);
void gfx_pixel(int x, int y, gfx_color_t c);
gfx_color_t gfx_get_pixel(int x, int y);
void gfx_fill_rect(int x, int y, int w, int h, gfx_color_t c);
void gfx_line(int x0, int y0, int x1, int y1, gfx_color_t c);
void gfx_rect(int x, int y, int w, int h, gfx_color_t c, bool fill);
void gfx_circle(int cx, int cy, int r, gfx_color_t c, bool fill);
/** 5x7 font, scale >= 1, '\n' = new line. If bg is non-NULL the glyph cells
 *  are filled with it. Returns width in pixels. */
int  gfx_text(int x, int y, const char *s, gfx_color_t fg, int scale, const gfx_color_t *bg);
int  gfx_text_width(const char *s, int scale);
/** Copy a w x h image of big-endian RGB565 pixels (2 bytes each, rows top to
 *  bottom) to (x, y), clipped to the screen. */
void gfx_blit_be565(int x, int y, int w, int h, const uint8_t *px);

bool gfx_dirty(void);
void gfx_flush(void);
/** The panel asleep or awake (lcd_sleep). Asleep nothing is sent to it:
 *  what is drawn meanwhile goes out when it wakes. */
void gfx_sleep(bool on);
