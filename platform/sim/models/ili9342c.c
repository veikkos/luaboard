#include "ili9342c.h"

#include <stdlib.h>
#include <string.h>

#include "sim_hw.h"

void ili9342c_init(ili9342c_t *m, int w, int h, int dc_pin)
{
    memset(m, 0, sizeof *m);
    pthread_mutex_init(&m->lock, NULL);
    m->w = w;
    m->h = h;
    m->dc_pin = dc_pin;
    m->gram = calloc((size_t)w * h, sizeof *m->gram);
    ili9342c_reset(m);
}

void ili9342c_reset(ili9342c_t *m)
{
    pthread_mutex_lock(&m->lock);
    m->sleeping = true;      /* power-on/reset state: sleep in, display off */
    m->display_on = false;
    m->inverted = false;
    m->bgr = false;
    m->colmod = 0x66;
    m->x0 = 0; m->x1 = m->w - 1;
    m->y0 = 0; m->y1 = m->h - 1;
    m->nparams = 0;
    m->pix_hi = -1;
    pthread_mutex_unlock(&m->lock);
}

static void exec_param_cmd(ili9342c_t *m)
{
    uint8_t *p = m->params;
    switch (m->cmd) {
    case 0x2A: /* CASET */
        if (m->nparams == 4) { m->x0 = (p[0] << 8) | p[1]; m->x1 = (p[2] << 8) | p[3]; }
        break;
    case 0x2B: /* RASET */
        if (m->nparams == 4) { m->y0 = (p[0] << 8) | p[1]; m->y1 = (p[2] << 8) | p[3]; }
        break;
    case 0x36: /* MADCTL: only the BGR bit is modelled */
        if (m->nparams == 1) m->bgr = (p[0] & 0x08) != 0;
        break;
    case 0x3A: /* COLMOD */
        if (m->nparams == 1) m->colmod = p[0];
        break;
    default:
        break;
    }
}

static void command(ili9342c_t *m, uint8_t c)
{
    m->cmd = c;
    m->nparams = 0;
    m->pix_hi = -1;
    switch (c) {
    case 0x01: /* SWRESET */
        m->sleeping = true; m->display_on = false; m->inverted = false; break;
    case 0x10: m->sleeping = true; break;
    case 0x11: m->sleeping = false; break;
    case 0x20: m->inverted = false; break;
    case 0x21: m->inverted = true; break;
    case 0x28: m->display_on = false; break;
    case 0x29: m->display_on = true; break;
    case 0x2C: /* RAMWR: restart at window origin */
        m->cx = m->x0; m->cy = m->y0; m->frames++; break;
    default: break;
    }
}

static void pixel(ili9342c_t *m, uint16_t v)
{
    if (m->cx >= 0 && m->cx < m->w && m->cy >= 0 && m->cy < m->h)
        m->gram[m->cy * m->w + m->cx] = v;
    if (++m->cx > m->x1) {
        m->cx = m->x0;
        if (++m->cy > m->y1) m->cy = m->y0;
    }
}

void ili9342c_spi_write(void *ctx, const uint8_t *d, size_t n)
{
    ili9342c_t *m = ctx;
    int dc = sim_gpio_output_level((gpio_num_t)m->dc_pin);
    pthread_mutex_lock(&m->lock);
    for (size_t i = 0; i < n; i++) {
        if (dc != 1) { command(m, d[i]); continue; }
        if (m->cmd == 0x2C) {                    /* pixel data, 2 bytes each */
            if (m->pix_hi < 0) m->pix_hi = d[i];
            else { pixel(m, (uint16_t)((m->pix_hi << 8) | d[i])); m->pix_hi = -1; }
        } else if (m->nparams < (int)sizeof m->params) {
            m->params[m->nparams++] = d[i];
            exec_param_cmd(m);
        }
    }
    pthread_mutex_unlock(&m->lock);
}

void ili9342c_view_rgb(ili9342c_t *m, uint8_t *rgb)
{
    pthread_mutex_lock(&m->lock);
    bool visible = m->display_on && !m->sleeping;
    for (int i = 0; i < m->w * m->h; i++) {
        uint16_t v = m->gram[i];
        /* This IPS panel shows inverted colors unless INVON is set, and its
           sub-pixels are BGR, so the controller must be told BGR order. */
        if (!m->inverted) v = (uint16_t)~v;
        int r = (v >> 11) & 0x1F, g = (v >> 5) & 0x3F, b = v & 0x1F;
        if (!m->bgr) { int t = r; r = b; b = t; }
        rgb[i * 3 + 0] = visible ? (uint8_t)((r * 527 + 23) >> 6) : 0;
        rgb[i * 3 + 1] = visible ? (uint8_t)((g * 259 + 33) >> 6) : 0;
        rgb[i * 3 + 2] = visible ? (uint8_t)((b * 527 + 23) >> 6) : 0;
    }
    pthread_mutex_unlock(&m->lock);
}
