#include "jpeg.h"

#include <stdlib.h>
#include <string.h>

#include "gfx.h"
#include "tjpgd.h"

#define POOL_SIZE 4096          /* TJpgDec work area (3.1 KB needed with JD_FASTDECODE 1) */

typedef struct {
    const uint8_t *data;
    size_t len, pos;
    int x, y;                   /* where the image goes on the screen */
} src_t;

static size_t in_func(JDEC *jd, uint8_t *buf, size_t n)
{
    src_t *s = jd->device;
    if (n > s->len - s->pos) n = s->len - s->pos;
    if (buf) memcpy(buf, s->data + s->pos, n);
    s->pos += n;
    return n;
}

static int out_func(JDEC *jd, void *bitmap, JRECT *r)
{
    src_t *s = jd->device;
    const uint16_t *px = bitmap;
    int w = r->right - r->left + 1, h = r->bottom - r->top + 1;
    /* gfx wants big-endian RGB565 bytes for blits; TJpgDec gives native uint16. */
    uint8_t row[2 * 16];        /* an MCU block is at most 16 px wide */
    if (w > 16) return 0;       /* can't happen with TJpgDec; stop rather than overrun */
    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) {
            row[2 * i] = (uint8_t)(px[j * w + i] >> 8);
            row[2 * i + 1] = (uint8_t)px[j * w + i];
        }
        gfx_blit_be565(s->x + r->left, s->y + r->top + j, w, 1, row);
    }
    return 1;
}

static const char *result_text(JRESULT rc)
{
    switch (rc) {
    case JDR_INP:  return "not a JPEG, or the data is cut short";
    case JDR_MEM1:
    case JDR_MEM2: return "JPEG too complex for the decoder";
    case JDR_FMT1: return "not a JPEG (or broken data)";
    case JDR_FMT2: return "unsupported JPEG variant";
    case JDR_FMT3: return "progressive or lossless JPEG (only baseline is supported)";
    default:       return "JPEG decoding failed";
    }
}

static const char *prepare(JDEC *jd, src_t *s, void **pool)
{
    *pool = malloc(POOL_SIZE);
    if (!*pool) return "out of memory";
    JRESULT rc = jd_prepare(jd, in_func, *pool, POOL_SIZE, s);
    if (rc != JDR_OK) { free(*pool); *pool = NULL; return result_text(rc); }
    return NULL;
}

const char *jpeg_size(const uint8_t *data, size_t len, int *w, int *h)
{
    JDEC jd;
    src_t s = { .data = data, .len = len };
    void *pool;
    const char *e = prepare(&jd, &s, &pool);
    if (e) return e;
    *w = jd.width;
    *h = jd.height;
    free(pool);
    return NULL;
}

const char *jpeg_draw(int x, int y, const uint8_t *data, size_t len, int scale, int *w, int *h)
{
    JDEC jd;
    src_t s = { .data = data, .len = len, .x = x, .y = y };
    void *pool;
    const char *e = prepare(&jd, &s, &pool);
    if (e) return e;
    *w = jd.width >> scale;
    *h = jd.height >> scale;
    JRESULT rc = jd_decomp(&jd, out_func, (uint8_t)scale);
    free(pool);
    return rc == JDR_OK ? NULL : result_text(rc);
}
