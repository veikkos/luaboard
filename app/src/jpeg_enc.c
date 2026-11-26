/* A baseline JPEG encoder (ITU T.81): 16x16 pixel units of four brightness
   blocks and one block each of the two color differences, every block
   through the cosine transform, divided by the quantization table and
   Huffman coded with the tables of the standard's annex K. */
#include "jpeg_enc.h"

#include <math.h>
#include <stdbool.h>
#include <string.h>

/* Annex K.1: the quantization tables at quality 50, rows left to right. */
static const uint8_t Q_LUMA[64] = {
    16, 11, 10, 16, 24,  40,  51,  61,  12, 12, 14, 19, 26,  58,  60,  55,
    14, 13, 16, 24, 40,  57,  69,  56,  14, 17, 22, 29, 51,  87,  80,  62,
    18, 22, 37, 56, 68,  109, 103, 77,  24, 35, 55, 64, 81,  104, 113, 92,
    49, 64, 78, 87, 103, 121, 120, 101, 72, 92, 95, 98, 112, 100, 103, 99,
};
static const uint8_t Q_CHROMA[64] = {
    17, 18, 24, 47, 99, 99, 99, 99, 18, 21, 26, 66, 99, 99, 99, 99,
    24, 26, 56, 99, 99, 99, 99, 99, 47, 66, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
};

/* The order coefficients are written in: where the n-th one sits in a block. */
static const uint8_t ZIGZAG[64] = {
    0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,  12, 19, 26, 33, 40, 48,
    41, 34, 27, 20, 13, 6,  7,  14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23,
    30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
};

/* Annex K.3: the Huffman tables, as the number of codes of each length 1..16
   and the symbols in code order. */
static const uint8_t DC_LUMA_BITS[16] = { 0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0 };
static const uint8_t DC_CHROMA_BITS[16] = { 0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0 };
static const uint8_t DC_VALS[12] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 };
static const uint8_t AC_LUMA_BITS[16] = { 0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7d };
static const uint8_t AC_LUMA_VALS[162] = {
    0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07, 0x22, 0x71,
    0x14, 0x32, 0x81, 0x91, 0xa1, 0x08, 0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0, 0x24, 0x33, 0x62, 0x72,
    0x82, 0x09, 0x0a, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x34, 0x35, 0x36, 0x37,
    0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59,
    0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x83,
    0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3,
    0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3,
    0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2,
    0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa,
};
static const uint8_t AC_CHROMA_BITS[16] = { 0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 0x77 };
static const uint8_t AC_CHROMA_VALS[162] = {
    0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71, 0x13, 0x22,
    0x32, 0x81, 0x08, 0x14, 0x42, 0x91, 0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33, 0x52, 0xf0, 0x15, 0x62, 0x72, 0xd1,
    0x0a, 0x16, 0x24, 0x34, 0xe1, 0x25, 0xf1, 0x17, 0x18, 0x19, 0x1a, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x35, 0x36,
    0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58,
    0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a,
    0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a,
    0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba,
    0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda,
    0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa,
};

typedef struct { uint16_t code[256]; uint8_t len[256]; } huff_t;

typedef struct {
    uint8_t *out;
    size_t cap, n;
    bool full;
    uint32_t bits;                       /* not written yet, the newest lowest */
    int nbits;
    huff_t dc[2], ac[2];                 /* 0 brightness, 1 color */
    float q[2][64];                      /* what a coefficient is divided by */
} enc_t;

static void put(enc_t *e, uint8_t b)
{
    if (e->n < e->cap) e->out[e->n++] = b;
    else e->full = true;
}

static void put_n(enc_t *e, const uint8_t *d, size_t n)
{
    for (size_t i = 0; i < n; i++) put(e, d[i]);
}

static void put16(enc_t *e, unsigned v)
{
    put(e, (uint8_t)(v >> 8));
    put(e, (uint8_t)v);
}

/* In the coded data a 0xFF byte is followed by a zero, so it isn't a marker. */
static void put_bits(enc_t *e, unsigned v, int n)
{
    e->bits = e->bits << n | (v & ((1u << n) - 1));
    e->nbits += n;
    while (e->nbits >= 8) {
        uint8_t b = (uint8_t)(e->bits >> (e->nbits - 8));
        put(e, b);
        if (b == 0xFF) put(e, 0);
        e->nbits -= 8;
    }
}

/* The codes of a table: in order of length, each one more than the last. */
static void huff_build(huff_t *h, const uint8_t bits[16], const uint8_t *vals)
{
    memset(h, 0, sizeof *h);
    unsigned code = 0;
    int k = 0;
    for (int len = 1; len <= 16; len++) {
        for (int i = 0; i < bits[len - 1]; i++, k++) {
            h->code[vals[k]] = (uint16_t)code++;
            h->len[vals[k]] = (uint8_t)len;
        }
        code <<= 1;
    }
}

static void put_dht(enc_t *e, int id, const uint8_t bits[16], const uint8_t *vals, int nvals)
{
    put16(e, 0xFFC4);
    put16(e, (unsigned)(2 + 1 + 16 + nvals));
    put(e, (uint8_t)id);                 /* high half: 0 DC, 1 AC */
    put_n(e, bits, 16);
    put_n(e, vals, (size_t)nvals);
}

/* A number as JPEG writes it: how many bits it takes, then those bits (a
   negative one as one less, so its top bit is 0). */
static int magnitude(int v, unsigned *bits)
{
    int a = v < 0 ? -v : v, n = 0;
    while (a >> n) n++;
    *bits = (unsigned)(v < 0 ? v - 1 : v) & ((1u << n) - 1);
    return n;
}

/* One 8x8 block (values around 0): transformed, quantized and written.
   Returns its first coefficient, which the next block of its kind is
   written as a difference to. */
static int put_block(enc_t *e, const float in[64], int kind, int last_dc)
{
    static float cosine[8][8];           /* [u][x] = cos((2x + 1) u pi / 16), scaled as the transform asks */
    if (cosine[0][0] == 0.0f)
        for (int u = 0; u < 8; u++)
            for (int x = 0; x < 8; x++)
                cosine[u][x] = cosf((float)((2 * x + 1) * u) * 3.14159265f / 16.0f) * (u ? 0.5f : 0.35355339f);
    float rows[64], f[64];
    for (int y = 0; y < 8; y++)
        for (int u = 0; u < 8; u++) {
            float s = 0;
            for (int x = 0; x < 8; x++) s += in[y * 8 + x] * cosine[u][x];
            rows[y * 8 + u] = s;
        }
    for (int u = 0; u < 8; u++)
        for (int v = 0; v < 8; v++) {
            float s = 0;
            for (int y = 0; y < 8; y++) s += rows[y * 8 + u] * cosine[v][y];
            f[v * 8 + u] = s;
        }

    int c[64];
    for (int i = 0; i < 64; i++) c[i] = (int)lroundf(f[ZIGZAG[i]] / e->q[kind][ZIGZAG[i]]);

    unsigned bits;
    int n = magnitude(c[0] - last_dc, &bits);
    put_bits(e, e->dc[kind].code[n], e->dc[kind].len[n]);
    if (n) put_bits(e, bits, n);

    const huff_t *ac = &e->ac[kind];
    int run = 0;
    for (int i = 1; i < 64; i++) {
        if (!c[i]) { run++; continue; }
        for (; run >= 16; run -= 16) put_bits(e, ac->code[0xF0], ac->len[0xF0]);     /* sixteen zeros */
        n = magnitude(c[i], &bits);
        put_bits(e, ac->code[run << 4 | n], ac->len[run << 4 | n]);
        put_bits(e, bits, n);
        run = 0;
    }
    if (run) put_bits(e, ac->code[0x00], ac->len[0x00]);                             /* nothing more in the block */
    return c[0];
}

size_t jpeg_encode_be565(const uint8_t *px, int w, int h, int quality, uint8_t *out, size_t cap)
{
    if (!px || w < 1 || h < 1 || w > 65535 || h > 65535) return 0;
    static enc_t e;                      /* (a kilobyte of tables: not on the stack) */
    memset(&e, 0, sizeof e);
    e.out = out;
    e.cap = cap;
    huff_build(&e.dc[0], DC_LUMA_BITS, DC_VALS);
    huff_build(&e.dc[1], DC_CHROMA_BITS, DC_VALS);
    huff_build(&e.ac[0], AC_LUMA_BITS, AC_LUMA_VALS);
    huff_build(&e.ac[1], AC_CHROMA_BITS, AC_CHROMA_VALS);

    /* The tables scaled for the quality, as the Independent JPEG Group's
       library does it: 50 is the standard's tables, 100 all ones. */
    if (quality < 1) quality = 1;
    if (quality > 100) quality = 100;
    int scale = quality < 50 ? 5000 / quality : 200 - 2 * quality;
    uint8_t qt[2][64];
    for (int t = 0; t < 2; t++)
        for (int i = 0; i < 64; i++) {
            int v = ((t ? Q_CHROMA : Q_LUMA)[i] * scale + 50) / 100;
            qt[t][i] = (uint8_t)(v < 1 ? 1 : v > 255 ? 255 : v);
            e.q[t][i] = (float)qt[t][i];
        }

    put16(&e, 0xFFD8);
    static const uint8_t JFIF[] = { 0xFF, 0xE0, 0, 16, 'J', 'F', 'I', 'F', 0, 1, 1, 0, 0, 1, 0, 1, 0, 0 };
    put_n(&e, JFIF, sizeof JFIF);
    for (int t = 0; t < 2; t++) {
        put16(&e, 0xFFDB);
        put16(&e, 2 + 1 + 64);
        put(&e, (uint8_t)t);
        for (int i = 0; i < 64; i++) put(&e, qt[t][ZIGZAG[i]]);
    }
    put16(&e, 0xFFC0);                   /* the frame: 8 bits, three parts, the first at twice the others' resolution */
    put16(&e, 2 + 6 + 3 * 3);
    put(&e, 8);
    put16(&e, (unsigned)h);
    put16(&e, (unsigned)w);
    put(&e, 3);
    static const uint8_t PARTS[] = { 1, 0x22, 0, 2, 0x11, 1, 3, 0x11, 1 };
    put_n(&e, PARTS, sizeof PARTS);
    put_dht(&e, 0x00, DC_LUMA_BITS, DC_VALS, 12);
    put_dht(&e, 0x10, AC_LUMA_BITS, AC_LUMA_VALS, 162);
    put_dht(&e, 0x01, DC_CHROMA_BITS, DC_VALS, 12);
    put_dht(&e, 0x11, AC_CHROMA_BITS, AC_CHROMA_VALS, 162);
    put16(&e, 0xFFDA);
    put16(&e, 2 + 1 + 3 * 2 + 3);
    static const uint8_t SCAN[] = { 3, 1, 0x00, 2, 0x11, 3, 0x11, 0, 63, 0 };
    put_n(&e, SCAN, sizeof SCAN);

    int dc[3] = { 0, 0, 0 };
    for (int my = 0; my < h && !e.full; my += 16)
        for (int mx = 0; mx < w; mx += 16) {
            /* The unit's 16x16 pixels, the picture's edge repeated past it. */
            float Y[4][64], cb[64], cr[64];
            memset(cb, 0, sizeof cb);
            memset(cr, 0, sizeof cr);
            for (int y = 0; y < 16; y++) {
                const uint8_t *row = px + (size_t)(my + y < h ? my + y : h - 1) * (size_t)w * 2;
                for (int x = 0; x < 16; x++) {
                    const uint8_t *p = row + (size_t)(mx + x < w ? mx + x : w - 1) * 2;
                    unsigned v = (unsigned)p[0] << 8 | p[1];
                    float r = (float)((v >> 11) << 3 | v >> 13);
                    float g = (float)((v >> 5 & 0x3F) << 2 | (v >> 9 & 0x03));
                    float b = (float)((v & 0x1F) << 3 | (v >> 2 & 0x07));
                    Y[(y >> 3) * 2 + (x >> 3)][(y & 7) * 8 + (x & 7)] = 0.299f * r + 0.587f * g + 0.114f * b - 128.0f;
                    int ci = (y >> 1) * 8 + (x >> 1);                               /* four pixels share a color */
                    cb[ci] += (-0.168736f * r - 0.331264f * g + 0.5f * b) / 4;
                    cr[ci] += (0.5f * r - 0.418688f * g - 0.081312f * b) / 4;
                }
            }
            for (int i = 0; i < 4; i++) dc[0] = put_block(&e, Y[i], 0, dc[0]);
            dc[1] = put_block(&e, cb, 1, dc[1]);
            dc[2] = put_block(&e, cr, 1, dc[2]);
        }
    if (e.nbits) put_bits(&e, 0x7F, 8 - e.nbits);                                   /* the last byte filled with ones */
    put16(&e, 0xFFD9);
    return e.full ? 0 : e.n;
}
