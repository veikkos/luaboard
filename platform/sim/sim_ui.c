#include "sim_ui.h"

#include <string.h>

#include "font5x7.h"

void ui_color(sim_ui_t *ui, uint32_t rgb, uint8_t a)
{
    SDL_SetRenderDrawColor(ui->r, (rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff, a);
}

void ui_fill(sim_ui_t *ui, int x, int y, int w, int h, uint32_t rgb)
{
    ui_color(ui, rgb, 255);
    SDL_Rect rc = { x, y, w, h };
    SDL_RenderFillRect(ui->r, &rc);
}

void ui_round_rect(sim_ui_t *ui, int x, int y, int w, int h, int rad, uint32_t rgb)
{
    if (rad * 2 > w) rad = w / 2;
    if (rad * 2 > h) rad = h / 2;
    ui_color(ui, rgb, 255);
    SDL_Rect mid = { x, y + rad, w, h - 2 * rad };
    SDL_RenderFillRect(ui->r, &mid);
    for (int dy = 0; dy < rad; dy++) {
        /* inset of the corner arc on this scanline */
        int yy = rad - dy;
        int inset = rad;
        while (inset > 0 && (rad - inset) * (rad - inset) + yy * yy <= rad * rad) inset--;
        SDL_Rect top = { x + inset, y + dy, w - 2 * inset, 1 };
        SDL_Rect bot = { x + inset, y + h - 1 - dy, w - 2 * inset, 1 };
        SDL_RenderFillRect(ui->r, &top);
        SDL_RenderFillRect(ui->r, &bot);
    }
}

void ui_circle(sim_ui_t *ui, int cx, int cy, int r, uint32_t rgb)
{
    ui_color(ui, rgb, 255);
    for (int dy = -r; dy <= r; dy++) {
        int dx = 0;
        while ((dx + 1) * (dx + 1) + dy * dy <= r * r) dx++;
        SDL_RenderDrawLine(ui->r, cx - dx, cy + dy, cx + dx, cy + dy);
    }
}

int ui_text(sim_ui_t *ui, int x, int y, const char *s, int scale, uint32_t rgb)
{
    int x0 = x;
    ui_color(ui, rgb, 255);
    for (; *s; s++, x += 6 * scale) {
        const uint8_t *g = font5x7_glyph(*s);
        for (int c = 0; c < 5; c++)
            for (int row = 0; row < 7; row++)
                if (g[c] & (1 << row)) {
                    SDL_Rect rc = { x + c * scale, y + row * scale, scale, scale };
                    SDL_RenderFillRect(ui->r, &rc);
                }
    }
    return x - x0 - scale;
}

int ui_text_w(const char *s, int scale)
{
    int n = (int)strlen(s);
    return n ? n * 6 * scale - scale : 0;
}

int ui_text_fit(const char *s, int want, int max_w)
{
    while (want > 1 && ui_text_w(s, want) > max_w) want--;
    return want < 1 ? 1 : want;
}

void ui_button(sim_ui_t *ui, ui_rect_t b, const char *label, int scale, uint32_t bg)
{
    ui_round_rect(ui, b.x, b.y, b.w, b.h, 5 * scale / 2, bg);
    ui_text(ui, b.x + (b.w - ui_text_w(label, scale)) / 2, b.y + (b.h - 7 * scale) / 2, label, scale, 0xffffff);
}

bool ui_hit(ui_rect_t r, int x, int y)
{
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

void ui_slider(sim_ui_t *ui, ui_rect_t t, int value, int max, uint32_t accent)
{
    int cy = t.y + t.h / 2;
    ui_round_rect(ui, t.x, cy - 2, t.w, 4, 2, 0x3a4048);
    int kx = t.x + (int)((long)value * (t.w - 14) / (max ? max : 1));
    ui_round_rect(ui, t.x, cy - 2, kx - t.x + 7, 4, 2, accent);
    ui_circle(ui, kx + 7, cy, 8, 0xe8ecf0);
}

int ui_slider_value(ui_rect_t t, int mx, int max)
{
    int v = (int)((long)(mx - t.x - 7) * max / (t.w - 14));
    return v < 0 ? 0 : v > max ? max : v;
}

void ui_lcd(sim_ui_t *ui, const uint8_t *rgb, int w, int h, int x, int y, float k)
{
    if (!ui->lcd || ui->lcd_w != w || ui->lcd_h != h) {
        if (ui->lcd) SDL_DestroyTexture(ui->lcd);
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");   /* nearest: crisp pixels */
        ui->lcd = SDL_CreateTexture(ui->r, SDL_PIXELFORMAT_RGB24, SDL_TEXTUREACCESS_STREAMING, w, h);
        ui->lcd_w = w;
        ui->lcd_h = h;
    }
    int s = ui->scale;
    SDL_UpdateTexture(ui->lcd, NULL, rgb, w * 3);
    float kc = k < 0 ? 0 : k > 1 ? 1 : k;
    uint8_t m = (uint8_t)(255 * kc);
    SDL_Rect dst = { x, y, w * s, h * s };

    /* Pixel grid darkens the last row/column of every cell (gaps between
       sub-pixel groups on a real panel). That alone makes the screen read
       much dimmer than the real thing, so the lit area is boosted by the
       inverse of the grid's average transmission: an extra additive pass
       of the same image. Saturated colours clip at 255, which is why the
       grid itself is kept fairly light. */
    int grid = ui->grid && s >= 2;
    uint8_t ga = s >= 3 ? 100 : 70;
    float gain = 1.0f;
    if (grid) {
        float t = 1.0f - ga / 255.0f;               /* transmission of a grid line */
        float n = (float)(s - 1);                   /* lit pixels per cell edge */
        float mean = (n * n + 2 * n * t + t * t) / (float)(s * s);
        gain = 1.0f / mean;
        if (gain > 2.0f) gain = 2.0f;
    }
    SDL_SetTextureBlendMode(ui->lcd, SDL_BLENDMODE_NONE);
    SDL_SetTextureColorMod(ui->lcd, m, m, m);   /* backlight */
    SDL_RenderCopy(ui->r, ui->lcd, NULL, &dst);
    if (gain > 1.0f) {
        uint8_t b = (uint8_t)(255 * kc * (gain - 1.0f));
        SDL_SetTextureBlendMode(ui->lcd, SDL_BLENDMODE_ADD);
        SDL_SetTextureColorMod(ui->lcd, b, b, b);
        SDL_RenderCopy(ui->r, ui->lcd, NULL, &dst);
        SDL_SetTextureBlendMode(ui->lcd, SDL_BLENDMODE_NONE);
    }

    if (grid) {
        SDL_SetRenderDrawBlendMode(ui->r, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(ui->r, 0, 0, 0, ga);
        for (int i = 1; i <= w; i++)
            SDL_RenderDrawLine(ui->r, x + i * s - 1, y, x + i * s - 1, y + h * s - 1);
        for (int j = 1; j <= h; j++)
            SDL_RenderDrawLine(ui->r, x, y + j * s - 1, x + w * s - 1, y + j * s - 1);
        SDL_SetRenderDrawBlendMode(ui->r, SDL_BLENDMODE_NONE);
    }
}
