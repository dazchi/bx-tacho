#include "display.h"

#include <string.h>
#include "board.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_lcd_io_i2c.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_ssd1306.h"
#include "esp_log.h"
#include "font5x7.h"

static const char *TAG = "display";

#define CMD_CONTRAST     0x81
#define CMD_CHARGE_PUMP  0x8D
#define CHARGE_PUMP_ON   0x14
#define CHARGE_PUMP_OFF  0x10

// SSD1306/1315 page layout: byte [page * 128 + x], bit (y % 8)
static uint8_t s_fb[DISPLAY_W * DISPLAY_H / 8];

static i2c_master_bus_handle_t s_bus;
static esp_lcd_panel_io_handle_t s_io;
static esp_lcd_panel_handle_t s_panel;
static bool s_present;
static bool s_on;

esp_err_t display_init(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = -1,
        .sda_io_num = PIN_OLED_SDA,
        .scl_io_num = PIN_OLED_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        // R21/R22 (4.7k) do the real work; the internal pull-ups only add a little margin
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &s_bus), TAG, "i2c bus");

    if (i2c_master_probe(s_bus, CONFIG_BXT_OLED_ADDR, 50) != ESP_OK) {
        ESP_LOGW(TAG, "no OLED at 0x%02X, running without display", CONFIG_BXT_OLED_ADDR);
        return ESP_ERR_NOT_FOUND;
    }

    esp_lcd_panel_io_i2c_config_t io_cfg = {
        .dev_addr = CONFIG_BXT_OLED_ADDR,
        .scl_speed_hz = CONFIG_BXT_OLED_I2C_HZ,
        .control_phase_bytes = 1,
        .dc_bit_offset = 6,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(s_bus, &io_cfg, &s_io), TAG, "panel io");

    esp_lcd_panel_ssd1306_config_t ssd_cfg = {.height = DISPLAY_H};
    esp_lcd_panel_dev_config_t panel_cfg = {
        .bits_per_pixel = 1,
        .reset_gpio_num = -1,
        .vendor_config = &ssd_cfg,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_ssd1306(s_io, &panel_cfg, &s_panel), TAG, "panel");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "init");

    s_present = true;
    display_clear();
    display_flush();
    display_power(true);
    return ESP_OK;
}

bool display_present(void)
{
    return s_present;
}

void display_clear(void)
{
    memset(s_fb, 0, sizeof(s_fb));
}

static inline void set_pixel(int x, int y)
{
    if (x >= 0 && x < DISPLAY_W && y >= 0 && y < DISPLAY_H) {
        s_fb[(y / 8) * DISPLAY_W + x] |= (uint8_t)(1 << (y % 8));
    }
}

void display_hline(int x0, int x1, int y)
{
    for (int x = x0; x <= x1; x++) {
        set_pixel(x, y);
    }
}

int display_text_width(const char *s, int scale)
{
    int n = (int)strlen(s);
    return n ? (n * 6 - 1) * scale : 0;
}

void display_text(int x, int y, const char *s, int scale)
{
    for (; *s; s++, x += 6 * scale) {
        const uint8_t *g = font5x7_glyph(*s);
        for (int col = 0; col < 5; col++) {
            for (int row = 0; row < 7; row++) {
                if (!(g[col] & (1 << row))) {
                    continue;
                }
                for (int dx = 0; dx < scale; dx++) {
                    for (int dy = 0; dy < scale; dy++) {
                        set_pixel(x + col * scale + dx, y + row * scale + dy);
                    }
                }
            }
        }
    }
}

esp_err_t display_flush(void)
{
    if (!s_present) {
        return ESP_ERR_INVALID_STATE;
    }
    return esp_lcd_panel_draw_bitmap(s_panel, 0, 0, DISPLAY_W, DISPLAY_H, s_fb);
}

void display_set_contrast(uint8_t level)
{
    if (s_present) {
        esp_lcd_panel_io_tx_param(s_io, CMD_CONTRAST, &level, 1);
    }
}

void display_power(bool on)
{
    if (!s_present || on == s_on) {
        return;
    }
    uint8_t pump = on ? CHARGE_PUMP_ON : CHARGE_PUMP_OFF;
    if (on) {
        esp_lcd_panel_io_tx_param(s_io, CMD_CHARGE_PUMP, &pump, 1);
        esp_lcd_panel_disp_on_off(s_panel, true);
    } else {
        esp_lcd_panel_disp_on_off(s_panel, false);
        esp_lcd_panel_io_tx_param(s_io, CMD_CHARGE_PUMP, &pump, 1);
    }
    s_on = on;
}
