#include "gfx.h"

#include <stdlib.h>
#include <string.h>

#include "board.h"
#include "esp_heap_caps.h"
#include "font5x7.h"
#include "lcd.h"
#include "text.h"

#define W BOARD_LCD_W
#define H BOARD_LCD_H

static uint16_t *s_fb;               /* W*H RGB565, in PSRAM on the device */
static int s_dx0 = W, s_dy0 = H, s_dx1 = -1, s_dy1 = -1;   /* dirty rectangle */

bool gfx_init(void)
{
    s_fb = heap_caps_calloc((size_t)W * H, sizeof *s_fb, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_fb) s_fb = calloc((size_t)W * H, sizeof *s_fb);
    return s_fb != NULL;
}

int gfx_width(void) { return W; }
int gfx_height(void) { return H; }
const uint16_t *gfx_framebuffer(void) { return s_fb; }

static inline void mark(int x0, int y0, int x1, int y1)
{
    if (x0 < s_dx0) s_dx0 = x0;
    if (y0 < s_dy0) s_dy0 = y0;
    if (x1 > s_dx1) s_dx1 = x1;
    if (y1 > s_dy1) s_dy1 = y1;
}

void gfx_fill(gfx_color_t c)
{
    if (!s_fb) return;                        /* no memory for it at boot: draw nothing, don't crash */
    for (int i = 0; i < W * H; i++) s_fb[i] = c;
    mark(0, 0, W - 1, H - 1);
}

void gfx_pixel(int x, int y, gfx_color_t c)
{
    if (!s_fb || (unsigned)x >= W || (unsigned)y >= H) return;
    s_fb[y * W + x] = c;
    mark(x, y, x, y);
}

gfx_color_t gfx_get_pixel(int x, int y)
{
    if (!s_fb || (unsigned)x >= W || (unsigned)y >= H) return 0;
    return s_fb[y * W + x];
}

void gfx_fill_rect(int x, int y, int w, int h, gfx_color_t c)
{
    /* Clip in 64 bits: x + w must not overflow for any int arguments. */
    int64_t x0 = x, y0 = y, x1 = (int64_t)x + w, y1 = (int64_t)y + h;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > W) x1 = W;
    if (y1 > H) y1 = H;
    if (!s_fb || x0 >= x1 || y0 >= y1) return;
    x = (int)x0; y = (int)y0; w = (int)(x1 - x0); h = (int)(y1 - y0);
    for (int j = 0; j < h; j++) {
        uint16_t *row = s_fb + (y + j) * W + x;
        for (int i = 0; i < w; i++) row[i] = c;
    }
    mark(x, y, x + w - 1, y + h - 1);
}

/* Callers keep coordinates within +-GFX_COORD_MAX, so the arithmetic below
   can't overflow and a line or circle costs at most ~100k steps. */
static int clampc(int v) { return v < -GFX_COORD_MAX ? -GFX_COORD_MAX : v > GFX_COORD_MAX ? GFX_COORD_MAX : v; }

void gfx_line(int x0, int y0, int x1, int y1, gfx_color_t c)
{
    x0 = clampc(x0); y0 = clampc(y0); x1 = clampc(x1); y1 = clampc(y1);
    if (y0 == y1) { gfx_fill_rect(x0 < x1 ? x0 : x1, y0, abs(x1 - x0) + 1, 1, c); return; }
    if (x0 == x1) { gfx_fill_rect(x0, y0 < y1 ? y0 : y1, 1, abs(y1 - y0) + 1, c); return; }
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        gfx_pixel(x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void gfx_rect(int x, int y, int w, int h, gfx_color_t c, bool fill)
{
    if (w <= 0 || h <= 0) return;
    x = clampc(x); y = clampc(y); w = clampc(w); h = clampc(h);
    if (fill) { gfx_fill_rect(x, y, w, h, c); return; }
    gfx_fill_rect(x, y, w, 1, c);
    gfx_fill_rect(x, y + h - 1, w, 1, c);
    gfx_fill_rect(x, y, 1, h, c);
    gfx_fill_rect(x + w - 1, y, 1, h, c);
}

void gfx_circle(int cx, int cy, int r, gfx_color_t c, bool fill)
{
    if (r < 0) return;
    cx = clampc(cx); cy = clampc(cy); r = clampc(r);
    if (fill) {
        int dx = r;
        int64_t rr = (int64_t)r * r + r;
        for (int dy = 0; dy <= r; dy++) {
            while (dx > 0 && (int64_t)dx * dx + (int64_t)dy * dy > rr) dx--;
            gfx_fill_rect(cx - dx, cy + dy, 2 * dx + 1, 1, c);
            if (dy) gfx_fill_rect(cx - dx, cy - dy, 2 * dx + 1, 1, c);
        }
        return;
    }
    int x = r, y = 0, err = 1 - r;
    while (x >= y) {
        gfx_pixel(cx + x, cy + y, c); gfx_pixel(cx + y, cy + x, c);
        gfx_pixel(cx - y, cy + x, c); gfx_pixel(cx - x, cy + y, c);
        gfx_pixel(cx - x, cy - y, c); gfx_pixel(cx - y, cy - x, c);
        gfx_pixel(cx + y, cy - x, c); gfx_pixel(cx + x, cy - y, c);
        y++;
        if (err < 0) err += 2 * y + 1;
        else { x--; err += 2 * (y - x) + 1; }
    }
}

/* ------------------------------------------------------------- text */

/* The glyphs (1..3) the font draws for the next character of s: its own,
   or for one it lacks, text_ascii's stand-in (ä as a, … as ...). */
static int glyphs(const char **s, const char *end, const uint8_t *g[3])
{
    uint32_t cp = text_next(s, end);
    if ((g[0] = font5x7_find(cp))) return 1;
    char one[2];
    int n = 0;
    for (const char *c = text_ascii(cp, one); *c && n < 3; c++) g[n++] = font5x7_glyph(*c);
    return n;
}

int gfx_text(int x, int y, const char *s, gfx_color_t fg, int scale, const gfx_color_t *bg)
{
    if (scale < 1) scale = 1;
    if (scale > 8) scale = 8;
    x = clampc(x); y = clampc(y);
    int cx = x, maxw = 0;
    const char *end = s + strlen(s);
    while (*s) {
        if (*s == '\n') {
            s++;
            if (cx - x > maxw) maxw = cx - x;
            cx = x;
            if (y < GFX_COORD_MAX) y += 8 * scale;
            continue;
        }
        const uint8_t *gl[3];
        for (int i = 0, n = glyphs(&s, end, gl); i < n; i++) {
            bool visible = cx < W && cx + 6 * scale > 0 && y < H && y + 8 * scale > 0;
            if (visible) {                    /* (long text off the screen is just measured) */
                if (bg) gfx_fill_rect(cx, y, 6 * scale, 8 * scale, *bg);
                const uint8_t *g = gl[i];
                for (int col = 0; col < FONT5X7_W; col++)
                    for (int row = 0; row < FONT5X7_H; row++)
                        if (g[col] & (1u << row))
                            gfx_fill_rect(cx + col * scale, y + row * scale, scale, scale, fg);
            }
            if (cx < GFX_COORD_MAX) cx += (FONT5X7_W + 1) * scale;
        }
    }
    if (cx - x > maxw) maxw = cx - x;
    return maxw > 0 ? maxw - scale : 0;
}

int gfx_text_width(const char *s, int scale)
{
    if (scale < 1) scale = 1;
    if (scale > 8) scale = 8;
    int w = 0, maxw = 0;
    const char *end = s + strlen(s);
    while (*s) {
        if (*s == '\n') { s++; if (w > maxw) maxw = w; w = 0; continue; }
        const uint8_t *gl[3];
        for (int i = 0, n = glyphs(&s, end, gl); i < n; i++)
            if (w < GFX_COORD_MAX) w += (FONT5X7_W + 1) * scale;
    }
    if (w > maxw) maxw = w;
    return maxw > 0 ? maxw - scale : 0;
}

void gfx_blit_be565(int x, int y, int w, int h, const uint8_t *px)
{
    if (!s_fb || w <= 0 || h <= 0) return;
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = (int64_t)x + w > W ? W : x + w, y1 = (int64_t)y + h > H ? H : y + h;
    if (x0 >= x1 || y0 >= y1) return;
    for (int j = y0; j < y1; j++) {
        const uint8_t *src = px + ((size_t)(j - y) * w + (x0 - x)) * 2;
        uint16_t *dst = s_fb + j * W + x0;
        for (int i = x0; i < x1; i++, src += 2) *dst++ = (uint16_t)(src[0] << 8 | src[1]);
    }
    mark(x0, y0, x1 - 1, y1 - 1);
}

bool gfx_dirty(void) { return s_dx1 >= 0; }

void gfx_flush(void)
{
    if (!s_fb || !gfx_dirty()) return;
    lcd_flush(s_fb, W, s_dx0, s_dy0, s_dx1, s_dy1);
    s_dx0 = W; s_dy0 = H; s_dx1 = -1; s_dy1 = -1;
}
