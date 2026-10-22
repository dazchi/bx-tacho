#pragma once

#include <stdint.h>

// 5x7 glyphs for ' ' (0x20) to '~' (0x7E), lowercase included. Each glyph is
// 5 column bytes, bit 0 = top row, bit 6 = the baseline; g j p q y also use
// bit 7, one row below it (so text takes 8 rows when it has those).
#define FONT5X7_FIRST  0x20
#define FONT5X7_LAST   0x7E

const uint8_t *font5x7_glyph(char c);
