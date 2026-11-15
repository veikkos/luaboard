/* UTF-8 text for the screen font (font5x7.h). */
#pragma once

#include <stdint.h>

/** Decode the UTF-8 character at `*p` (before `end`) and step past it.
 *  Broken UTF-8 gives U+FFFD and steps one byte. */
uint32_t text_next(const char **p, const char *end);

/** An ASCII stand-in (1..3 characters) for code point `cp`, for characters
 *  the font has no glyph for: letters with accents (U+00C0..U+017F) as the
 *  plain letter, a few punctuation marks as their ASCII look-alikes,
 *  anything else as "?". `one` holds the result when it's a single
 *  character. */
const char *text_ascii(uint32_t cp, char one[2]);

/** `cp` as UTF-8 into out (1..4 bytes, no terminator); returns the length.
 *  Surrogates and values past U+10FFFF give U+FFFD. */
int text_utf8(uint32_t cp, char out[4]);

/** Past the first `n` characters of the string s (or at its end). */
const char *text_skip(const char *s, int n);
