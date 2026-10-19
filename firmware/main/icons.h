// 1-bit bitmaps for the OLED, drawn with display_bitmap().
// Row-major, MSB = leftmost pixel, rows padded to whole bytes.
#pragma once

#include <stdint.h>

#define ICON_BATTERY_OFF_W 48
#define ICON_BATTERY_OFF_H 48
extern const uint8_t icon_battery_off_48[];
