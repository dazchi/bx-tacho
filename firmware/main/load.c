// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Darren Chi
#include "load.h"

#include <limits.h>
#include <stdlib.h>
#include "board.h"
#include "esp_check.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sense_adc.h"
#include "settings.h"

static const char *TAG = "load";

// The sensor responds with a time constant of about 0.3 ms at the low light a
// black launcher reflects, both ways (LED on and LED off): 1 ms is about 97 %
#define LED_SETTLE_US   1000
// Flash: measured on a black launcher (the least light, the slowest sensor
// response), 400 us reached 75 % of the polled level, 600 us 91 %, 800 us 98 %
#define FLASH_SETTLE_US 800
// A flash reads the ambient level first, with the LED already off: it must have
// been off this long, or the light still fading out is taken for ambient light
#define FLASH_DARK_US   1000

// Hysteresis and debounce (HANDOVER.md 6.2)
#define LOADED_PCT      60
#define RELEASED_PCT    40
#define DEBOUNCE_COUNT  4

// While empty, slowly track the baseline so drift does not cause false loads
#define BASELINE_TRACK_PCT  20
#define BASELINE_DIV        32  // EMA weight 1/32


static SemaphoreHandle_t s_lock;
static bool s_forced;   // diag: LED held on
static int64_t s_led_off_us;   // when the LED last went off

static bool s_loaded;
static int s_streak;
static int s_last_pct;

static launcher_t s_launcher;   // black until a reading says otherwise
static int s_launcher_streak;

esp_err_t load_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_NO_MEM, TAG, "mutex");

    gpio_config_t out = {
        .pin_bit_mask = BIT64(PIN_LOAD_IR_LED),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&out), TAG, "led");
    gpio_set_level(PIN_LOAD_IR_LED, 0);
    return ESP_OK;
}

int load_measure_raw(int samples, int *on_mv, int *off_mv)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);

    gpio_set_level(PIN_LOAD_IR_LED, 0);
    esp_rom_delay_us(LED_SETTLE_US);
    int off = sense_adc_read_mv(ADC_CH_LOAD_SENSE, samples);

    gpio_set_level(PIN_LOAD_IR_LED, 1);
    esp_rom_delay_us(LED_SETTLE_US);
    int on = sense_adc_read_mv(ADC_CH_LOAD_SENSE, samples);

    gpio_set_level(PIN_LOAD_IR_LED, s_forced ? 1 : 0);
    s_led_off_us = esp_timer_get_time();
    xSemaphoreGive(s_lock);

    if (on_mv) {
        *on_mv = on;
    }
    if (off_mv) {
        *off_mv = off;
    }
    if (off < 0 || on < 0) {
        return INT_MIN;
    }
    return on - off;
}

int load_measure(int samples)
{
    return load_measure_raw(samples, NULL, NULL);
}

int load_flash_sample_raw(int *on_mv, int *off_mv)
{
    return load_flash_sample_settle_raw(FLASH_SETTLE_US, on_mv, off_mv);
}

int load_flash_sample_settle_raw(int settle_us, int *on_mv, int *off_mv)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    // Ambient first: the LED has been off since the last flash or poll. Flashes
    // are spaced so it normally has; right after a poll it may not have yet.
    int64_t dark_us = esp_timer_get_time() - s_led_off_us;
    if (dark_us < FLASH_DARK_US) {
        esp_rom_delay_us(FLASH_DARK_US - (uint32_t)dark_us);
    }
    int off = sense_adc_read_mv(ADC_CH_LOAD_SENSE, 2);
    gpio_set_level(PIN_LOAD_IR_LED, 1);
    esp_rom_delay_us(settle_us);
    int on = sense_adc_read_mv(ADC_CH_LOAD_SENSE, 2);
    gpio_set_level(PIN_LOAD_IR_LED, s_forced ? 1 : 0);
    s_led_off_us = esp_timer_get_time();
    xSemaphoreGive(s_lock);

    if (on_mv) {
        *on_mv = on;
    }
    if (off_mv) {
        *off_mv = off;
    }
    if (off < 0 || on < 0) {
        return INT_MIN;
    }
    return on - off;
}

void load_led_off(void)
{
    load_led_force(false);
}

void load_led_force(bool on)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_forced = on;
    gpio_set_level(PIN_LOAD_IR_LED, on ? 1 : 0);
    s_led_off_us = esp_timer_get_time();
    xSemaphoreGive(s_lock);
}

bool load_led_forced(void)
{
    return s_forced;
}

void load_classifier_reset(bool loaded)
{
    s_loaded = loaded;
    s_streak = 0;
}

int load_last_percent(void)
{
    return s_last_pct;
}

launcher_t load_launcher(void)
{
    return s_launcher;
}

// Position of a reading between a launcher's empty (0) and loaded (100) levels,
// or false without a usable calibration for it
static bool cal_percent(launcher_t launcher, int diff_mv, int *pct)
{
    const launcher_cal_t *cal = settings_cal(launcher);
    int span = cal->load_loaded - cal->load_empty;
    if (!cal->load_cal_valid || abs(span) < LOAD_MIN_CONTRAST_MV) {
        return false;
    }
    *pct = (diff_mv - cal->load_empty) * 100 / span;
    return true;
}

launcher_t load_track_launcher(int diff_mv, bool may_switch)
{
    if (diff_mv == INT_MIN) {
        return s_launcher;
    }
    // Above the white launcher's loaded level: only a white launcher reads that
    // high (a top on it, or its pin). Below the black one's released level: only
    // a black launcher (or none) reads that low. In between (an empty white
    // launcher, the pin pushed up on a black one) it stays as it is.
    int pct;
    launcher_t seen = s_launcher;
    if (cal_percent(LAUNCHER_WHITE, diff_mv, &pct) && pct > LOADED_PCT) {
        seen = LAUNCHER_WHITE;
    } else if (cal_percent(LAUNCHER_BLACK, diff_mv, &pct) && pct < RELEASED_PCT) {
        seen = LAUNCHER_BLACK;
    }
    if (seen == s_launcher || !may_switch) {
        s_launcher_streak = 0;
    } else if (++s_launcher_streak >= DEBOUNCE_COUNT) {
        s_launcher = seen;
        s_launcher_streak = 0;
        // Start from where this reading lies on the new launcher's scale, not from
        // the old state, so the switch itself isn't taken for a load or a release
        const launcher_cal_t *cal = settings_cal(seen);
        int span = cal->load_loaded - cal->load_empty;
        s_loaded = cal->load_cal_valid && abs(span) >= LOAD_MIN_CONTRAST_MV &&
                   (diff_mv - cal->load_empty) * 100 / span > 50;
        s_streak = 0;
    }
    return s_launcher;
}

bool load_classify(int diff_mv, bool may_switch)
{
    launcher_cal_t *cfg = settings_cal(load_track_launcher(diff_mv, may_switch));
    int span = cfg->load_loaded - cfg->load_empty;

    if (!cfg->load_cal_valid || diff_mv == INT_MIN || abs(span) < LOAD_MIN_CONTRAST_MV) {
        s_loaded = false;
        s_streak = 0;
        return false;
    }

    // Works whichever way the signal moves when the center part rises
    int pct = (diff_mv - cfg->load_empty) * 100 / span;
    s_last_pct = pct;

    bool crossing = s_loaded ? (pct < RELEASED_PCT) : (pct > LOADED_PCT);
    if (crossing) {
        if (++s_streak >= DEBOUNCE_COUNT) {
            s_loaded = !s_loaded;
            s_streak = 0;
        }
    } else {
        s_streak = 0;
    }

    // Re-learn the empty baseline while idle (RAM only; NVS keeps the original),
    // only near the empty level: readings off the launcher (about 0 mV) would
    // otherwise drag it down, and it couldn't come back up
    if (!s_loaded && pct > -BASELINE_TRACK_PCT && pct < BASELINE_TRACK_PCT && s_launcher_streak == 0) {
        cfg->load_empty += (diff_mv - cfg->load_empty) / BASELINE_DIV;
    }
    return s_loaded;
}
