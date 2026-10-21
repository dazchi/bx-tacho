#include "load.h"

#include <limits.h>
#include <stdlib.h>
#include "board.h"
#include "esp_check.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sense_adc.h"
#include "settings.h"

static const char *TAG = "load";

// ITR8307 rise time is ~20 us; 1 ms is generous and also lets C10 (100 us) settle
#define LED_SETTLE_US   1000
// Flash: 4 time constants of C10 (about 98 %), well inside LOAD_FLASH_LEN_US
#define FLASH_SETTLE_US 400

// Hysteresis and debounce (HANDOVER.md 6.2)
#define LOADED_PCT      60
#define RELEASED_PCT    40
#define DEBOUNCE_COUNT  4

// While empty, slowly track the baseline so drift does not cause false loads
#define BASELINE_TRACK_PCT  20
#define BASELINE_DIV        32  // EMA weight 1/32

static SemaphoreHandle_t s_lock;
static bool s_forced;   // diag: LED held on

static bool s_loaded;
static int s_streak;
static int s_last_pct;

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
    xSemaphoreTake(s_lock, portMAX_DELAY);
    // Ambient first: the LED has been off since the last flash
    int off = sense_adc_read_mv(ADC_CH_LOAD_SENSE, 2);
    gpio_set_level(PIN_LOAD_IR_LED, 1);
    esp_rom_delay_us(FLASH_SETTLE_US);
    int on = sense_adc_read_mv(ADC_CH_LOAD_SENSE, 2);
    gpio_set_level(PIN_LOAD_IR_LED, s_forced ? 1 : 0);
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

bool load_classify(int diff_mv)
{
    settings_t *cfg = settings_get();
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

    // Re-learn the empty baseline while idle (RAM only; NVS keeps the original)
    if (!s_loaded && pct < BASELINE_TRACK_PCT) {
        cfg->load_empty += (diff_mv - cfg->load_empty) / BASELINE_DIV;
    }
    return s_loaded;
}
