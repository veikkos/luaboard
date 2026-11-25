#include "ili9342.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sim_hw.h"

void ili9342_init(ili9342_t *m, int w, int h, int dc_pin)
{
    memset(m, 0, sizeof *m);
    pthread_mutex_init(&m->lock, NULL);
    m->w = w;
    m->h = h;
    m->dc_pin = dc_pin;
    m->variant = 'C';
    m->gram = calloc((size_t)w * h, sizeof *m->gram);
    ili9342_reset(m);
}

void ili9342_set_variant(ili9342_t *m, char variant)
{
    pthread_mutex_lock(&m->lock);
    m->variant = variant;
    m->unlocked = false;
    m->gamma = 0;
    m->warned = false;
    pthread_mutex_unlock(&m->lock);
}

static bool set_up(const ili9342_t *m) { return m->unlocked && m->gamma == 3; }

void ili9342_reset(ili9342_t *m)
{
    pthread_mutex_lock(&m->lock);
    m->sleeping = true;      /* power-on/reset state: sleep in, display off */
    m->display_on = false;
    m->inverted = false;
    m->bgr = false;
    m->unlocked = false;
    m->gamma = 0;
    m->colmod = 0x66;
    m->x0 = 0; m->x1 = m->w - 1;
    m->y0 = 0; m->y1 = m->h - 1;
    m->nparams = 0;
    m->pix_hi = -1;
    pthread_mutex_unlock(&m->lock);
}

static void exec_param_cmd(ili9342_t *m)
{
    uint8_t *p = m->params;
    bool e = m->variant == 'E';
    if (m->cmd == (e ? 0xDD : 0xC8)) {           /* this variant's key to the extended commands */
        if (e) m->unlocked = m->nparams == 1 && p[0] == 0x01;
        else if (m->nparams == 3) m->unlocked = p[0] == 0xFF && p[1] == 0x93 && p[2] == 0x42;
    } else if (m->unlocked && m->nparams == 15) {
        if (m->cmd == (e ? 0xE4 : 0xE0)) m->gamma |= 1;
        if (m->cmd == (e ? 0xE5 : 0xE1)) m->gamma |= 2;
    }
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

static void command(ili9342_t *m, uint8_t c)
{
    m->cmd = c;
    m->nparams = 0;
    m->pix_hi = -1;
    switch (c) {
    case 0x01: /* SWRESET */
        m->sleeping = true; m->display_on = false; m->inverted = false;
        m->unlocked = false; m->gamma = 0; break;
    case 0x10: m->sleeping = true; break;
    case 0x11: m->sleeping = false; break;
    case 0x20: m->inverted = false; break;
    case 0x21: m->inverted = true; break;
    case 0x28: m->display_on = false; break;
    case 0x29:
        m->display_on = true;
        if (!set_up(m) && !m->warned) {
            m->warned = true;
            fprintf(stderr, "ILI9342%c: display on without this panel's set-up (its key, then both gamma curves): "
                    "the picture is off\n", m->variant);
        }
        break;
    case 0x2C: /* RAMWR: restart at window origin */
        m->cx = m->x0; m->cy = m->y0; m->frames++; break;
    default: break;
    }
}

static void pixel(ili9342_t *m, uint16_t v)
{
    if (m->cx >= 0 && m->cx < m->w && m->cy >= 0 && m->cy < m->h)
        m->gram[m->cy * m->w + m->cx] = v;
    if (++m->cx > m->x1) {
        m->cx = m->x0;
        if (++m->cy > m->y1) m->cy = m->y0;
    }
}

void ili9342_spi_write(void *ctx, const uint8_t *d, size_t n)
{
    ili9342_t *m = ctx;
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

void ili9342_view_rgb(ili9342_t *m, uint8_t *rgb)
{
    pthread_mutex_lock(&m->lock);
    bool visible = m->display_on && !m->sleeping;
    bool washed = !set_up(m);
    for (int i = 0; i < m->w * m->h; i++) {
        uint16_t v = m->gram[i];
        /* This IPS panel shows inverted colors unless INVON is set, and its
           sub-pixels are BGR, so the controller must be told BGR order. */
        if (!m->inverted) v = (uint16_t)~v;
        int r = (v >> 11) & 0x1F, g = (v >> 5) & 0x3F, b = v & 0x1F;
        if (!m->bgr) { int t = r; r = b; b = t; }
        uint8_t *o = rgb + i * 3;
        o[0] = (uint8_t)((r * 527 + 23) >> 6);
        o[1] = (uint8_t)((g * 259 + 33) >> 6);
        o[2] = (uint8_t)((b * 527 + 23) >> 6);
        for (int k = 0; k < 3; k++) {
            if (washed) o[k] = (uint8_t)(o[k] / 2 + 64);
            if (!visible) o[k] = 0;
        }
    }
    pthread_mutex_unlock(&m->lock);
}
