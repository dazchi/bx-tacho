#pragma once

#include <stdint.h>

// 5x7 glyphs for ' ' (0x20) to 'Z' (0x5A). Each glyph is 5 column bytes,
// bit 0 = top row. Lowercase is drawn as uppercase.
#define FONT5X7_FIRST  0x20
#define FONT5X7_LAST   0x5A

const uint8_t *font5x7_glyph(char c);
