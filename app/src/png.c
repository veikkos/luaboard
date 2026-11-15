/* PNG decoding onto the framebuffer. See png.h.
 *
 * The zlib stream across the IDAT chunks goes through miniz's tinfl (in the
 * ESP32-S3's ROM on the device, third_party/miniz in the simulator) into a
 * 32 KB wrapping window. Each finished scanline is unfiltered against the
 * one above, turned into RGBA and added to one accumulator per output
 * column; every `scale` rows those become a row of output pixels. */
#include "png.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "gfx.h"
#include "miniz.h"

#define PNG_MAX_W 4096
#define PNG_MAX_H 8192          /* rows above the screen are decoded too: keep that bounded */

static const uint8_t SIGNATURE[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };

typedef struct {
    uint32_t w, h;
    uint8_t depth, ctype;
    int channels;                    /* samples a pixel */
    int bpp;                         /* bytes a pixel for the filters (at least 1) */
    size_t stride;                   /* bytes a scanline, without its filter byte */
    uint8_t plte[256 * 3];
    uint8_t plte_a[256];             /* tRNS for palettes; 255 where not given */
    bool has_key;                    /* tRNS for grey and RGB: a colour that is transparent */
    uint16_t key[3];
} hdr_t;

static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

/* The header: signature and IHDR, which must come first. */
static const char *parse_ihdr(const uint8_t *data, size_t len, hdr_t *hd)
{
    if (len < 8 || memcmp(data, SIGNATURE, 8) != 0) return "not a PNG";
    if (len < 8 + 8 + 13 || be32(data + 8) != 13 || memcmp(data + 12, "IHDR", 4) != 0) return "broken data";
    const uint8_t *p = data + 16;
    hd->w = be32(p);
    hd->h = be32(p + 4);
    hd->depth = p[8];
    hd->ctype = p[9];
    if (p[10] != 0 || p[11] != 0) return "broken data";        /* compression, filter method */
    if (p[12] == 1) return "interlaced PNGs aren't supported";
    if (p[12] != 0) return "broken data";
    if (hd->w == 0 || hd->h == 0 || hd->h > 0x7FFFFFFF) return "broken data";
    switch (hd->ctype) {
    case 0: hd->channels = 1; if (hd->depth != 1 && hd->depth != 2 && hd->depth != 4 && hd->depth != 8 && hd->depth != 16) return "broken data"; break;
    case 3: hd->channels = 1; if (hd->depth != 1 && hd->depth != 2 && hd->depth != 4 && hd->depth != 8) return "broken data"; break;
    case 2: hd->channels = 3; if (hd->depth != 8 && hd->depth != 16) return "broken data"; break;
    case 4: hd->channels = 2; if (hd->depth != 8 && hd->depth != 16) return "broken data"; break;
    case 6: hd->channels = 4; if (hd->depth != 8 && hd->depth != 16) return "broken data"; break;
    default: return "broken data";
    }
    hd->bpp = hd->channels * hd->depth / 8;
    if (hd->bpp < 1) hd->bpp = 1;
    hd->stride = ((size_t)hd->w * hd->channels * hd->depth + 7) / 8;
    return NULL;
}

const char *png_size(const uint8_t *data, size_t len, int *w, int *h)
{
    hdr_t hd;
    const char *e = parse_ihdr(data, len, &hd);
    if (e) return e;
    if (hd.w > 0x7FFFFFFF) return "broken data";
    *w = (int)hd.w;
    *h = (int)hd.h;
    return NULL;
}

/* ---- decoding ----------------------------------------------------------- */

typedef struct {
    const hdr_t *hd;
    int x0, y0, scale, ow, oh;
    uint8_t *line, *prev;            /* filter byte + scanline */
    size_t fill;                     /* bytes of `line` so far */
    uint32_t row;                    /* scanlines done */
    int rows_in;                     /* scanlines in the accumulators */
    uint32_t *acc;                   /* per output column: r*a, g*a, b*a, a */
    uint32_t xs, xe;                 /* source columns that land on the screen */
    bool bad, below;                 /* broken data; the rest is below the screen */
} dec_t;

static uint8_t paeth(int a, int b, int c)
{
    int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
    return (uint8_t)(pa <= pb && pa <= pc ? a : pb <= pc ? b : c);
}

static bool unfilter(dec_t *d)
{
    uint8_t *r = d->line + 1, *u = d->prev + 1;
    size_t n = d->hd->stride, bpp = (size_t)d->hd->bpp;
    switch (d->line[0]) {
    case 0: break;
    case 1: for (size_t i = bpp; i < n; i++) r[i] += r[i - bpp]; break;
    case 2: for (size_t i = 0; i < n; i++) r[i] += u[i]; break;
    case 3:
        for (size_t i = 0; i < n; i++) r[i] += (uint8_t)(((i >= bpp ? r[i - bpp] : 0) + u[i]) / 2);
        break;
    case 4:
        for (size_t i = 0; i < n; i++)
            r[i] += paeth(i >= bpp ? r[i - bpp] : 0, u[i], i >= bpp ? u[i - bpp] : 0);
        break;
    default: return false;
    }
    return true;
}

/* Sample `i` (0-based, of all samples in the row) at the header's depth. */
static uint16_t sample(const dec_t *d, const uint8_t *r, size_t i)
{
    switch (d->hd->depth) {
    case 16: return (uint16_t)(r[2 * i] << 8 | r[2 * i + 1]);
    case 8:  return r[i];
    default: {
        int depth = d->hd->depth, per = 8 / depth;
        int shift = 8 - depth * (int)(i % (size_t)per + 1);
        return (uint16_t)((r[i / (size_t)per] >> shift) & ((1 << depth) - 1));
    }
    }
}

/* A sample as 8 bits. */
static uint8_t to8(const dec_t *d, uint16_t v)
{
    switch (d->hd->depth) {
    case 16: return (uint8_t)(v >> 8);
    case 8:  return (uint8_t)v;
    default: return (uint8_t)(v * 255 / ((1 << d->hd->depth) - 1));
    }
}

/* Every `scale` scanlines (or at the last): a row of output pixels. */
static void emit(dec_t *d)
{
    int oy = (int)(d->row - 1) / d->scale;
    int y = d->y0 + oy;
    for (int ox = 0; ox < d->ow; ox++) {
        uint32_t *a = d->acc + 4 * ox;
        int x = d->x0 + ox;
        uint32_t asum = a[3];
        if (asum && x >= 0 && x < gfx_width() && y >= 0 && y < gfx_height()) {
            int cols = (int)d->hd->w - ox * d->scale;
            if (cols > d->scale) cols = d->scale;
            uint32_t n = (uint32_t)(cols * d->rows_in);
            uint32_t alpha = (asum + n / 2) / n;
            uint32_t r = (a[0] + asum / 2) / asum, g = (a[1] + asum / 2) / asum, b = (a[2] + asum / 2) / asum;
            if (alpha < 255) {
                uint32_t dst = gfx_to_rgb888(gfx_get_pixel(x, y));
                uint32_t dr = dst >> 16 & 0xFF, dg = dst >> 8 & 0xFF, db = dst & 0xFF;
                r = (r * alpha + dr * (255 - alpha) + 127) / 255;
                g = (g * alpha + dg * (255 - alpha) + 127) / 255;
                b = (b * alpha + db * (255 - alpha) + 127) / 255;
            }
            gfx_pixel(x, y, gfx_rgb((uint8_t)r, (uint8_t)g, (uint8_t)b));
        }
        a[0] = a[1] = a[2] = a[3] = 0;
    }
    d->rows_in = 0;
}

/* A finished scanline: unfilter, to RGBA, into the accumulators. */
static void scanline(dec_t *d)
{
    if (!unfilter(d)) { d->bad = true; return; }
    const hdr_t *hd = d->hd;
    const uint8_t *r = d->line + 1;
    for (uint32_t x = d->xs; x < d->xe; x++) {
        uint8_t c[4];
        size_t s = (size_t)x * (size_t)hd->channels;
        switch (hd->ctype) {
        case 0: {
            uint16_t v = sample(d, r, s);
            c[0] = c[1] = c[2] = to8(d, v);
            c[3] = hd->has_key && v == hd->key[0] ? 0 : 255;
            break;
        }
        case 2: {
            uint16_t v0 = sample(d, r, s), v1 = sample(d, r, s + 1), v2 = sample(d, r, s + 2);
            c[0] = to8(d, v0); c[1] = to8(d, v1); c[2] = to8(d, v2);
            c[3] = hd->has_key && v0 == hd->key[0] && v1 == hd->key[1] && v2 == hd->key[2] ? 0 : 255;
            break;
        }
        case 3: {
            uint16_t i = sample(d, r, s);
            c[0] = hd->plte[3 * i]; c[1] = hd->plte[3 * i + 1]; c[2] = hd->plte[3 * i + 2];
            c[3] = hd->plte_a[i];
            break;
        }
        case 4:
            c[0] = c[1] = c[2] = to8(d, sample(d, r, s));
            c[3] = to8(d, sample(d, r, s + 1));
            break;
        default:
            c[0] = to8(d, sample(d, r, s)); c[1] = to8(d, sample(d, r, s + 1));
            c[2] = to8(d, sample(d, r, s + 2)); c[3] = to8(d, sample(d, r, s + 3));
            break;
        }
        uint32_t *a = d->acc + 4 * (x / (uint32_t)d->scale);
        a[0] += (uint32_t)c[0] * c[3];
        a[1] += (uint32_t)c[1] * c[3];
        a[2] += (uint32_t)c[2] * c[3];
        a[3] += c[3];
    }
    d->row++;
    d->rows_in++;
    if (d->rows_in == d->scale || d->row == hd->h) {
        emit(d);
        if (d->y0 + (int)(d->row - 1) / d->scale >= gfx_height() - 1) d->below = true;
    }
    uint8_t *t = d->prev; d->prev = d->line; d->line = t;
    d->fill = 0;
}

/* Inflated bytes: into scanlines. */
static void feed(dec_t *d, const uint8_t *p, size_t n)
{
    size_t full = 1 + d->hd->stride;
    while (n && !d->bad && !d->below && d->row < d->hd->h) {
        size_t k = full - d->fill;
        if (k > n) k = n;
        memcpy(d->line + d->fill, p, k);
        d->fill += k; p += k; n -= k;
        if (d->fill == full) scanline(d);
    }
}

const char *png_draw(int x, int y, const uint8_t *data, size_t len, int shift, int *w, int *h)
{
    hdr_t hd;
    memset(&hd, 0, sizeof hd);
    memset(hd.plte_a, 255, sizeof hd.plte_a);
    const char *e = parse_ihdr(data, len, &hd);
    if (e) return e;
    if (hd.w > PNG_MAX_W) return "PNG too wide";
    if (hd.h > PNG_MAX_H) return "PNG too big";
    int scale = 1 << shift;
    dec_t d = { .hd = &hd, .x0 = x, .y0 = y, .scale = scale };
    d.ow = (int)((hd.w + (uint32_t)scale - 1) / (uint32_t)scale);
    d.oh = (int)((hd.h + (uint32_t)scale - 1) / (uint32_t)scale);
    *w = d.ow;
    *h = d.oh;
    if (y >= gfx_height() || x >= gfx_width() || x + d.ow <= 0 || y + d.oh <= 0) return NULL;   /* nothing on the screen */
    /* the source columns whose output is on the screen */
    int first = x < 0 ? -x : 0, last = x + d.ow > gfx_width() ? gfx_width() - x : d.ow;
    d.xs = (uint32_t)first * (uint32_t)scale;
    d.xe = (uint32_t)last * (uint32_t)scale;
    if (d.xe > hd.w) d.xe = hd.w;

    tinfl_decompressor *inf = malloc(sizeof *inf);
    uint8_t *dict = malloc(TINFL_LZ_DICT_SIZE);
    d.line = malloc(1 + hd.stride);
    d.prev = calloc(1, 1 + hd.stride);
    d.acc = calloc((size_t)d.ow * 4, sizeof *d.acc);
    if (!inf || !dict || !d.line || !d.prev || !d.acc) { e = "out of memory"; goto out; }
    tinfl_init(inf);

    size_t pos = 8, dict_ofs = 0;
    bool done = false;
    while (!done && !e) {
        if (pos + 12 > len) { e = "broken data"; break; }    /* cut short before IEND */
        uint32_t clen = be32(data + pos);
        const uint8_t *type = data + pos + 4, *body = data + pos + 8;
        if (clen > len - pos - 12) { e = "broken data"; break; }
        if (!memcmp(type, "PLTE", 4)) {
            if (clen % 3 || clen > sizeof hd.plte) { e = "broken data"; break; }
            memcpy(hd.plte, body, clen);
        } else if (!memcmp(type, "tRNS", 4)) {
            if (hd.ctype == 3) memcpy(hd.plte_a, body, clen < 256 ? clen : 256);
            else if (hd.ctype == 0 && clen >= 2) { hd.has_key = true; hd.key[0] = (uint16_t)(body[0] << 8 | body[1]); }
            else if (hd.ctype == 2 && clen >= 6) {
                hd.has_key = true;
                for (int i = 0; i < 3; i++) hd.key[i] = (uint16_t)(body[2 * i] << 8 | body[2 * i + 1]);
            }
            if (hd.depth < 16 && hd.has_key)                /* the key is at the image's depth */
                for (int i = 0; i < 3; i++) hd.key[i] &= (uint16_t)((1u << hd.depth) - 1);
        } else if (!memcmp(type, "IDAT", 4)) {
            const uint8_t *in = body;
            size_t left = clen;
            for (;;) {
                size_t in_n = left, out_n = TINFL_LZ_DICT_SIZE - dict_ofs;
                tinfl_status st = tinfl_decompress(inf, in, &in_n, dict, dict + dict_ofs, &out_n,
                                                   TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_HAS_MORE_INPUT);
                in += in_n; left -= in_n;
                feed(&d, dict + dict_ofs, out_n);
                dict_ofs = (dict_ofs + out_n) & (TINFL_LZ_DICT_SIZE - 1);
                if (d.bad || st < 0) { e = "broken data"; break; }
                if (st == TINFL_STATUS_DONE || d.below) { done = true; break; }
                if (st == TINFL_STATUS_NEEDS_MORE_INPUT && left == 0) break;
            }
        } else if (!memcmp(type, "IEND", 4)) {
            e = "broken data";                               /* the image data ended early */
        }
        pos += 12 + clen;
    }
    if (!e && d.row < hd.h && !d.below) e = "broken data";

out:
    free(inf);
    free(dict);
    free(d.line);
    free(d.prev);
    free(d.acc);
    return e;
}
