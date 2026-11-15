#include "font5x7.h"

#include <stddef.h>

const uint8_t font5x7[(FONT5X7_LAST - FONT5X7_FIRST + 1) * FONT5X7_W] = {
    0x00,0x00,0x00,0x00,0x00, /* ' ' */  0x00,0x00,0x5F,0x00,0x00, /* !   */
    0x00,0x07,0x00,0x07,0x00, /* "   */  0x14,0x7F,0x14,0x7F,0x14, /* #   */
    0x24,0x2A,0x7F,0x2A,0x12, /* $   */  0x23,0x13,0x08,0x64,0x62, /* %   */
    0x36,0x49,0x55,0x22,0x50, /* &   */  0x00,0x05,0x03,0x00,0x00, /* '   */
    0x00,0x1C,0x22,0x41,0x00, /* (   */  0x00,0x41,0x22,0x1C,0x00, /* )   */
    0x08,0x2A,0x1C,0x2A,0x08, /* *   */  0x08,0x08,0x3E,0x08,0x08, /* +   */
    0x00,0x50,0x30,0x00,0x00, /* ,   */  0x08,0x08,0x08,0x08,0x08, /* -   */
    0x00,0x60,0x60,0x00,0x00, /* .   */  0x20,0x10,0x08,0x04,0x02, /* /   */
    0x3E,0x51,0x49,0x45,0x3E, /* 0   */  0x00,0x42,0x7F,0x40,0x00, /* 1   */
    0x42,0x61,0x51,0x49,0x46, /* 2   */  0x21,0x41,0x45,0x4B,0x31, /* 3   */
    0x18,0x14,0x12,0x7F,0x10, /* 4   */  0x27,0x45,0x45,0x45,0x39, /* 5   */
    0x3C,0x4A,0x49,0x49,0x30, /* 6   */  0x01,0x71,0x09,0x05,0x03, /* 7   */
    0x36,0x49,0x49,0x49,0x36, /* 8   */  0x06,0x49,0x49,0x29,0x1E, /* 9   */
    0x00,0x36,0x36,0x00,0x00, /* :   */  0x00,0x56,0x36,0x00,0x00, /* ;   */
    0x08,0x14,0x22,0x41,0x00, /* <   */  0x14,0x14,0x14,0x14,0x14, /* =   */
    0x00,0x41,0x22,0x14,0x08, /* >   */  0x02,0x01,0x51,0x09,0x06, /* ?   */
    0x32,0x49,0x79,0x41,0x3E, /* @   */  0x7E,0x11,0x11,0x11,0x7E, /* A   */
    0x7F,0x49,0x49,0x49,0x36, /* B   */  0x3E,0x41,0x41,0x41,0x22, /* C   */
    0x7F,0x41,0x41,0x22,0x1C, /* D   */  0x7F,0x49,0x49,0x49,0x41, /* E   */
    0x7F,0x09,0x09,0x01,0x01, /* F   */  0x3E,0x41,0x41,0x51,0x32, /* G   */
    0x7F,0x08,0x08,0x08,0x7F, /* H   */  0x00,0x41,0x7F,0x41,0x00, /* I   */
    0x20,0x40,0x41,0x3F,0x01, /* J   */  0x7F,0x08,0x14,0x22,0x41, /* K   */
    0x7F,0x40,0x40,0x40,0x40, /* L   */  0x7F,0x02,0x04,0x02,0x7F, /* M   */
    0x7F,0x04,0x08,0x10,0x7F, /* N   */  0x3E,0x41,0x41,0x41,0x3E, /* O   */
    0x7F,0x09,0x09,0x09,0x06, /* P   */  0x3E,0x41,0x51,0x21,0x5E, /* Q   */
    0x7F,0x09,0x19,0x29,0x46, /* R   */  0x46,0x49,0x49,0x49,0x31, /* S   */
    0x01,0x01,0x7F,0x01,0x01, /* T   */  0x3F,0x40,0x40,0x40,0x3F, /* U   */
    0x1F,0x20,0x40,0x20,0x1F, /* V   */  0x7F,0x20,0x18,0x20,0x7F, /* W   */
    0x63,0x14,0x08,0x14,0x63, /* X   */  0x03,0x04,0x78,0x04,0x03, /* Y   */
    0x61,0x51,0x49,0x45,0x43, /* Z   */  0x00,0x7F,0x41,0x41,0x00, /* [   */
    0x02,0x04,0x08,0x10,0x20, /* \   */  0x00,0x41,0x41,0x7F,0x00, /* ]   */
    0x04,0x02,0x01,0x02,0x04, /* ^   */  0x40,0x40,0x40,0x40,0x40, /* _   */
    0x00,0x01,0x02,0x04,0x00, /* `   */  0x20,0x54,0x54,0x54,0x78, /* a   */
    0x7F,0x48,0x44,0x44,0x38, /* b   */  0x38,0x44,0x44,0x44,0x20, /* c   */
    0x38,0x44,0x44,0x48,0x7F, /* d   */  0x38,0x54,0x54,0x54,0x18, /* e   */
    0x08,0x7E,0x09,0x01,0x02, /* f   */  0x08,0x14,0x54,0x54,0x3C, /* g   */
    0x7F,0x08,0x04,0x04,0x78, /* h   */  0x00,0x44,0x7D,0x40,0x00, /* i   */
    0x20,0x40,0x44,0x3D,0x00, /* j   */  0x00,0x7F,0x10,0x28,0x44, /* k   */
    0x00,0x41,0x7F,0x40,0x00, /* l   */  0x7C,0x04,0x18,0x04,0x78, /* m   */
    0x7C,0x08,0x04,0x04,0x78, /* n   */  0x38,0x44,0x44,0x44,0x38, /* o   */
    0x7C,0x14,0x14,0x14,0x08, /* p   */  0x08,0x14,0x14,0x18,0x7C, /* q   */
    0x7C,0x08,0x04,0x04,0x08, /* r   */  0x48,0x54,0x54,0x54,0x20, /* s   */
    0x04,0x3F,0x44,0x40,0x20, /* t   */  0x3C,0x40,0x40,0x20,0x7C, /* u   */
    0x1C,0x20,0x40,0x20,0x1C, /* v   */  0x3C,0x40,0x30,0x40,0x3C, /* w   */
    0x44,0x28,0x10,0x28,0x44, /* x   */  0x0C,0x50,0x50,0x50,0x3C, /* y   */
    0x44,0x64,0x54,0x4C,0x44, /* z   */  0x00,0x08,0x36,0x41,0x00, /* {   */
    0x00,0x00,0x7F,0x00,0x00, /* |   */  0x00,0x41,0x36,0x08,0x00, /* }   */
    0x02,0x01,0x02,0x04,0x02, /* ~   */
};

/* ------------------------------------------------ beyond ASCII
 * Latin-1 (U+00A0..U+00FF), a few Latin Extended-A letters (Finnish
 * Š and Ž, Œ, Ÿ) and €, drawn as 7 rows of 5 (X = lit). Capitals with an
 * accent are a row or two shorter to make room for it, so some come close
 * to the small letter: Ä, Ë and Ö have their dots wider apart than ä, ë and
 * ö, while Ô and ô or Ž and ž look the same. Å and å have a ring of their
 * own above the letter. */

enum {
    R______, R_____X, R____X_, R____XX, R___X__, R___X_X, R___XX_, R___XXX,
    R__X___, R__X__X, R__X_X_, R__X_XX, R__XX__, R__XX_X, R__XXX_, R__XXXX,
    R_X____, R_X___X, R_X__X_, R_X__XX, R_X_X__, R_X_X_X, R_X_XX_, R_X_XXX,
    R_XX___, R_XX__X, R_XX_X_, R_XX_XX, R_XXX__, R_XXX_X, R_XXXX_, R_XXXXX,
};

/* column c (0 = left) of a glyph given as 7 row patterns, top row in bit 0 */
#define COL_(c, r0, r1, r2, r3, r4, r5, r6)                                   \
    (uint8_t)(((r0) >> (4 - c) & 1) | ((r1) >> (4 - c) & 1) << 1 |           \
              ((r2) >> (4 - c) & 1) << 2 | ((r3) >> (4 - c) & 1) << 3 |      \
              ((r4) >> (4 - c) & 1) << 4 | ((r5) >> (4 - c) & 1) << 5 |      \
              ((r6) >> (4 - c) & 1) << 6)
#define GLYPH_(cp, ...) \
    { cp, { COL_(0, __VA_ARGS__), COL_(1, __VA_ARGS__), COL_(2, __VA_ARGS__), \
            COL_(3, __VA_ARGS__), COL_(4, __VA_ARGS__) } }
#define G(cp, a, b, c, d, e, f, g) GLYPH_(cp, R_##a, R_##b, R_##c, R_##d, R_##e, R_##f, R_##g)

typedef struct {
    uint16_t cp;
    uint8_t col[FONT5X7_W];
} ext_glyph_t;

static const ext_glyph_t EXT[] = {   /* sorted by code point */
    G(0x00A0, /*   */ _____, _____, _____, _____, _____, _____, _____),
    G(0x00A1, /* ¡ */ __X__, _____, __X__, __X__, __X__, __X__, __X__),
    G(0x00A2, /* ¢ */ __X__, _XXXX, X_X__, X_X__, X_X__, _XXXX, __X__),
    G(0x00A3, /* £ */ __XX_, _X__X, _X___, XXXX_, _X___, _X___, XXXXX),
    G(0x00A4, /* ¤ */ _____, X___X, _XXX_, _X_X_, _XXX_, X___X, _____),
    G(0x00A5, /* ¥ */ X___X, _X_X_, XXXXX, __X__, XXXXX, __X__, __X__),
    G(0x00A6, /* ¦ */ __X__, __X__, __X__, _____, __X__, __X__, __X__),
    G(0x00A7, /* § */ _XXX_, X____, _XXX_, X___X, _XXX_, ____X, _XXX_),
    G(0x00A8, /* ¨ */ _X_X_, _____, _____, _____, _____, _____, _____),
    G(0x00A9, /* © */ _XXX_, X___X, X_XXX, X_X_X, X_XXX, X___X, _XXX_),
    G(0x00AA, /* ª */ _XXX_, ____X, _XXXX, X___X, _XXXX, _____, XXXXX),
    G(0x00AB, /* « */ _____, __X_X, _X_X_, X_X__, _X_X_, __X_X, _____),
    G(0x00AC, /* ¬ */ _____, _____, XXXXX, ____X, ____X, _____, _____),
    G(0x00AD, /* - */ _____, _____, _____, XXXXX, _____, _____, _____),
    G(0x00AE, /* ® */ _XXX_, X___X, XXX_X, XX_XX, XXX_X, XX_XX, _XXX_),
    G(0x00AF, /* ¯ */ XXXXX, _____, _____, _____, _____, _____, _____),
    G(0x00B0, /* ° */ _XX__, X__X_, X__X_, _XX__, _____, _____, _____),
    G(0x00B1, /* ± */ __X__, __X__, XXXXX, __X__, __X__, _____, XXXXX),
    G(0x00B2, /* ² */ _XX__, X__X_, __X__, _X___, XXXX_, _____, _____),
    G(0x00B3, /* ³ */ XXX__, ___X_, _XX__, ___X_, XXX__, _____, _____),
    G(0x00B4, /* ´ */ ___X_, __X__, _____, _____, _____, _____, _____),
    G(0x00B5, /* µ */ _____, X___X, X___X, X___X, XX_XX, X_XX_, X____),
    G(0x00B6, /* ¶ */ _XXXX, XXX_X, XXX_X, _XX_X, __X_X, __X_X, __X_X),
    G(0x00B7, /* · */ _____, _____, _____, __X__, _____, _____, _____),
    G(0x00B8, /* ¸ */ _____, _____, _____, _____, _____, __X__, _XX__),
    G(0x00B9, /* ¹ */ _X___, XX___, _X___, _X___, XXX__, _____, _____),
    G(0x00BA, /* º */ _XXX_, X___X, X___X, X___X, _XXX_, _____, XXXXX),
    G(0x00BB, /* » */ _____, X_X__, _X_X_, __X_X, _X_X_, X_X__, _____),
    G(0x00BC, /* ¼ */ X___X, X__X_, X_X__, _XX_X, X_XXX, ____X, ____X),
    G(0x00BD, /* ½ */ X___X, X__X_, X_X__, _X_XX, X___X, ___X_, __XXX),
    G(0x00BE, /* ¾ */ XX__X, _X_X_, XXX__, _XX_X, X_XXX, ____X, ____X),
    G(0x00BF, /* ¿ */ __X__, _____, __X__, _X___, X____, X___X, _XXX_),
    G(0x00C0, /* À */ _X___, __X__, _XXX_, X___X, XXXXX, X___X, X___X),
    G(0x00C1, /* Á */ ___X_, __X__, _XXX_, X___X, XXXXX, X___X, X___X),
    G(0x00C2, /* Â */ __X__, _X_X_, _XXX_, X___X, XXXXX, X___X, X___X),
    G(0x00C3, /* Ã */ _XX_X, X_XX_, _XXX_, X___X, XXXXX, X___X, X___X),
    G(0x00C4, /* Ä */ X___X, _____, _XXX_, X___X, XXXXX, X___X, X___X),
    G(0x00C5, /* Å */ __X__, _X_X_, __X__, _XXX_, X___X, XXXXX, X___X),
    G(0x00C6, /* Æ */ _XXXX, X_X__, X_X__, XXXXX, X_X__, X_X__, X_XXX),
    G(0x00C7, /* Ç */ _XXX_, X___X, X____, X___X, _XXX_, __X__, _XX__),
    G(0x00C8, /* È */ _X___, __X__, XXXXX, X____, XXXX_, X____, XXXXX),
    G(0x00C9, /* É */ ___X_, __X__, XXXXX, X____, XXXX_, X____, XXXXX),
    G(0x00CA, /* Ê */ __X__, _X_X_, XXXXX, X____, XXXX_, X____, XXXXX),
    G(0x00CB, /* Ë */ X___X, _____, XXXXX, X____, XXXX_, X____, XXXXX),
    G(0x00CC, /* Ì */ _X___, __X__, _XXX_, __X__, __X__, __X__, _XXX_),
    G(0x00CD, /* Í */ ___X_, __X__, _XXX_, __X__, __X__, __X__, _XXX_),
    G(0x00CE, /* Î */ __X__, _X_X_, _XXX_, __X__, __X__, __X__, _XXX_),
    G(0x00CF, /* Ï */ _X_X_, _____, _XXX_, __X__, __X__, __X__, _XXX_),
    G(0x00D0, /* Ð */ _XXX_, _X__X, _X__X, XXX_X, _X__X, _X__X, _XXX_),
    G(0x00D1, /* Ñ */ _XX_X, X_XX_, X___X, XX__X, X_X_X, X__XX, X___X),
    G(0x00D2, /* Ò */ _X___, __X__, _XXX_, X___X, X___X, X___X, _XXX_),
    G(0x00D3, /* Ó */ ___X_, __X__, _XXX_, X___X, X___X, X___X, _XXX_),
    G(0x00D4, /* Ô */ __X__, _X_X_, _XXX_, X___X, X___X, X___X, _XXX_),
    G(0x00D5, /* Õ */ _XX_X, X_XX_, _XXX_, X___X, X___X, X___X, _XXX_),
    G(0x00D6, /* Ö */ X___X, _____, _XXX_, X___X, X___X, X___X, _XXX_),
    G(0x00D7, /* × */ _____, X___X, _X_X_, __X__, _X_X_, X___X, _____),
    G(0x00D8, /* Ø */ ____X, _XXX_, X__XX, X_X_X, XX__X, _XXX_, X____),
    G(0x00D9, /* Ù */ _X___, __X__, X___X, X___X, X___X, X___X, _XXX_),
    G(0x00DA, /* Ú */ ___X_, __X__, X___X, X___X, X___X, X___X, _XXX_),
    G(0x00DB, /* Û */ __X__, _X_X_, X___X, X___X, X___X, X___X, _XXX_),
    G(0x00DC, /* Ü */ _X_X_, _____, X___X, X___X, X___X, X___X, _XXX_),
    G(0x00DD, /* Ý */ ___X_, __X__, X___X, _X_X_, __X__, __X__, __X__),
    G(0x00DE, /* Þ */ X____, XXXX_, X___X, X___X, XXXX_, X____, X____),
    G(0x00DF, /* ß */ _XX__, X__X_, X_X__, X__X_, X___X, X___X, X_XX_),
    G(0x00E0, /* à */ _X___, __X__, _XXX_, ____X, _XXXX, X___X, _XXXX),
    G(0x00E1, /* á */ ___X_, __X__, _XXX_, ____X, _XXXX, X___X, _XXXX),
    G(0x00E2, /* â */ __X__, _X_X_, _XXX_, ____X, _XXXX, X___X, _XXXX),
    G(0x00E3, /* ã */ _XX_X, X_XX_, _XXX_, ____X, _XXXX, X___X, _XXXX),
    G(0x00E4, /* ä */ _X_X_, _____, _XXX_, ____X, _XXXX, X___X, _XXXX),
    G(0x00E5, /* å */ __X__, _X_X_, __X__, _XXXX, X___X, X___X, _XXXX),
    G(0x00E6, /* æ */ _____, _____, XX_X_, __X_X, XXXXX, X_X__, XX_XX),
    G(0x00E7, /* ç */ _____, _____, _XXXX, X____, X____, _XXXX, __XX_),
    G(0x00E8, /* è */ _X___, __X__, _XXX_, X___X, XXXXX, X____, _XXX_),
    G(0x00E9, /* é */ ___X_, __X__, _XXX_, X___X, XXXXX, X____, _XXX_),
    G(0x00EA, /* ê */ __X__, _X_X_, _XXX_, X___X, XXXXX, X____, _XXX_),
    G(0x00EB, /* ë */ _X_X_, _____, _XXX_, X___X, XXXXX, X____, _XXX_),
    G(0x00EC, /* ì */ _X___, __X__, _XX__, __X__, __X__, __X__, _XXX_),
    G(0x00ED, /* í */ ___X_, __X__, _XX__, __X__, __X__, __X__, _XXX_),
    G(0x00EE, /* î */ __X__, _X_X_, _XX__, __X__, __X__, __X__, _XXX_),
    G(0x00EF, /* ï */ _X_X_, _____, _XX__, __X__, __X__, __X__, _XXX_),
    G(0x00F0, /* ð */ _X_X_, __X__, _X_X_, _XXXX, X___X, X___X, _XXX_),
    G(0x00F1, /* ñ */ _XX_X, X_XX_, X_XX_, XX__X, X___X, X___X, X___X),
    G(0x00F2, /* ò */ _X___, __X__, _XXX_, X___X, X___X, X___X, _XXX_),
    G(0x00F3, /* ó */ ___X_, __X__, _XXX_, X___X, X___X, X___X, _XXX_),
    G(0x00F4, /* ô */ __X__, _X_X_, _XXX_, X___X, X___X, X___X, _XXX_),
    G(0x00F5, /* õ */ _XX_X, X_XX_, _XXX_, X___X, X___X, X___X, _XXX_),
    G(0x00F6, /* ö */ _X_X_, _____, _XXX_, X___X, X___X, X___X, _XXX_),
    G(0x00F7, /* ÷ */ _____, __X__, _____, XXXXX, _____, __X__, _____),
    G(0x00F8, /* ø */ _____, ____X, _XXX_, X__XX, X_X_X, XX__X, XXXX_),
    G(0x00F9, /* ù */ _X___, __X__, X___X, X___X, X___X, X__XX, _XX_X),
    G(0x00FA, /* ú */ ___X_, __X__, X___X, X___X, X___X, X__XX, _XX_X),
    G(0x00FB, /* û */ __X__, _X_X_, X___X, X___X, X___X, X__XX, _XX_X),
    G(0x00FC, /* ü */ _X_X_, _____, X___X, X___X, X___X, X__XX, _XX_X),
    G(0x00FD, /* ý */ ___X_, __X__, X___X, X___X, _XXXX, ____X, _XXX_),
    G(0x00FE, /* þ */ X____, X____, XXXX_, X___X, X___X, XXXX_, X____),
    G(0x00FF, /* ÿ */ _X_X_, _____, X___X, X___X, _XXXX, ____X, _XXX_),
    G(0x0152, /* Œ */ _XXXX, X_X__, X_X__, X_XXX, X_X__, X_X__, _XXXX),
    G(0x0153, /* œ */ _____, _____, _X_X_, X_X_X, X_XXX, X_X__, _X_XX),
    G(0x0160, /* Š */ _X_X_, __X__, _XXXX, X____, _XXX_, ____X, XXXX_),
    G(0x0161, /* š */ _X_X_, __X__, _XXX_, X____, _XXX_, ____X, XXXX_),
    G(0x0178, /* Ÿ */ _X_X_, _____, X___X, _X_X_, __X__, __X__, __X__),
    G(0x017D, /* Ž */ _X_X_, __X__, XXXXX, ___X_, __X__, _X___, XXXXX),
    G(0x017E, /* ž */ _X_X_, __X__, XXXXX, ___X_, __X__, _X___, XXXXX),
    G(0x20AC, /* € */ __XXX, _X___, XXXX_, _X___, XXXX_, _X___, __XXX),
};

const uint8_t *font5x7_find(uint32_t cp)
{
    if (cp >= FONT5X7_FIRST && cp <= FONT5X7_LAST) return &font5x7[(cp - FONT5X7_FIRST) * FONT5X7_W];
    size_t lo = 0, hi = sizeof EXT / sizeof EXT[0];
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (EXT[mid].cp == cp) return EXT[mid].col;
        if (EXT[mid].cp < cp) lo = mid + 1;
        else hi = mid;
    }
    return NULL;
}
