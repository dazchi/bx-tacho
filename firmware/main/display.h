// 128x64 SSD1315 OLED over I2C, drawn from a local framebuffer.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define DISPLAY_W 128
#define DISPLAY_H 64

esp_err_t display_init(void);
bool display_present(void);

void display_clear(void);
// Draw text with the 5x7 font magnified by `scale` (glyph cell = 6*scale wide).
void display_text(int x, int y, const char *s, int scale);
void display_hline(int x0, int x1, int y);
// Width in pixels of `s` drawn at `scale`
int display_text_width(const char *s, int scale);

esp_err_t display_flush(void);

void display_set_contrast(uint8_t level);
// Off = display off and charge pump off (lowest current)
void display_power(bool on);
