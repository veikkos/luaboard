/* Generic SDL2 drawing helpers for the simulator window. */
#pragma once

#include <SDL.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    SDL_Renderer *r;
    int scale;                /* LCD pixel -> canvas pixels (integer, for the pixel grid) */
    bool grid;                /* draw the LCD's pixel grid (at scale >= 2) */
    bool settings_open;       /* the settings overlay is shown; the board may set it */
    char tty_path[256];
    SDL_Texture *lcd;         /* streaming texture for the LCD image */
    int lcd_w, lcd_h;
} sim_ui_t;

void ui_color(sim_ui_t *ui, uint32_t rgb, uint8_t alpha);
void ui_fill(sim_ui_t *ui, int x, int y, int w, int h, uint32_t rgb);
void ui_round_rect(sim_ui_t *ui, int x, int y, int w, int h, int radius, uint32_t rgb);
void ui_circle(sim_ui_t *ui, int cx, int cy, int r, uint32_t rgb);
/** 5x7 bitmap text; returns width in pixels. */
int  ui_text(sim_ui_t *ui, int x, int y, const char *s, int scale, uint32_t rgb);
/** Width of ui_text() output without drawing it. */
int  ui_text_w(const char *s, int scale);
/** The largest text scale up to want (at least 1) that keeps s within max_w. */
int  ui_text_fit(const char *s, int want, int max_w);
/** Horizontal slider; value 0..max. Returns true and updates *value while dragged. */
typedef struct { int x, y, w, h; } ui_rect_t;
bool ui_hit(ui_rect_t r, int x, int y);
void ui_slider(sim_ui_t *ui, ui_rect_t track, int value, int max, uint32_t accent);
int  ui_slider_value(ui_rect_t track, int mx, int max);
/** A rounded button with its label centred, text at the given scale. */
void ui_button(sim_ui_t *ui, ui_rect_t b, const char *label, int scale, uint32_t bg);
/** Draw an RGB888 LCD image at (x, y), ui->scale canvas pixels per LCD pixel,
 *  dimmed by the backlight level, with a pixel grid when ui->grid and scale >= 2. */
void ui_lcd(sim_ui_t *ui, const uint8_t *rgb, int w, int h, int x, int y, float brightness);
