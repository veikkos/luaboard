/*
 * PNG images onto the framebuffer, decoded while they inflate: only a few
 * rows and the 32 KB inflate window in memory, whatever the image's size.
 * Shrinking by 2, 4 or 8 averages the pixels; alpha blends over what is on
 * the screen. Every colour type and bit depth; no interlaced images.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

/** The image's size from its header. NULL, or why not. */
const char *png_size(const uint8_t *data, size_t len, int *w, int *h);

/** Draw at (x, y), shrunk by 1 << shift (0..3); *w, *h get the drawn size.
 *  NULL, or why it couldn't (what was drawn before a broken part stays). */
const char *png_draw(int x, int y, const uint8_t *data, size_t len, int shift, int *w, int *h);
