/* JPEG drawing for disp.jpeg() (TJpgDec, third_party/tjpgd). */
#pragma once

#include <stddef.h>
#include <stdint.h>

/** Decode a baseline JPEG to the framebuffer at (x, y), clipped. scale is
 *  0..3 (1/1, 1/2, 1/4, 1/8). Returns NULL and the drawn size, or an error. */
const char *jpeg_draw(int x, int y, const uint8_t *data, size_t len, int scale, int *w, int *h);
/** Image size without decoding. */
const char *jpeg_size(const uint8_t *data, size_t len, int *w, int *h);
