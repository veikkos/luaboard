/* JPEG writing for camera.jpeg(): baseline, the colors at half resolution
   (4:2:0), the standard's own tables. */
#pragma once

#include <stddef.h>
#include <stdint.h>

/** A w x h picture (RGB565, the high byte first, rows top to bottom) as a
 *  JPEG into out. quality 1..100. Returns its size, or 0 if cap bytes
 *  aren't enough. */
size_t jpeg_encode_be565(const uint8_t *px, int w, int h, int quality, uint8_t *out, size_t cap);
