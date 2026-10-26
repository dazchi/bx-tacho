// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Darren Chi
#include "load.h"

#include <limits.h>
#include <stdlib.h>
#include "board.h"
#include "driver/gptimer.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "hal/gpio_ll.h"
#include "sense_adc.h"
#include "settings.h"

// The edge flash runs in interrupts that may fire while the flash cache is off
// (an NVS write): what they call must be in IRAM
#if !CONFIG_ADC_ONESHOT_CTRL_FUNC_IN_IRAM || !CONFIG_GPTIMER_CTRL_FUNC_IN_IRAM || !CONFIG_GPTIMER_ISR_CACHE_SAFE
#error "Set CONFIG_ADC_ONESHOT_CTRL_FUNC_IN_IRAM, CONFIG_GPTIMER_CTRL_FUNC_IN_IRAM and CONFIG_GPTIMER_ISR_CACHE_SAFE (sdkconfig.defaults)"
#endif

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

// Edge flash (load_edge_flash_arm): started at a rising RPM edge, the start of
// the disc's white part, and over (LED off) by EDGE_FLASH_WHITE_PCT of the
// white part, taken as half the latest valid edge interval. The LED's light
// reaching the RPM sensor then only adds to an already high signal, so it can't
// make or move a rising edge. The settling time is cut to fit (30000 rpm: 2 ms
// a revolution, off within 0.9 ms of the edge), down to EDGE_FLASH_MIN_SETTLE_US.
#define EDGE_FLASH_WHITE_PCT     90
#define EDGE_FLASH_MIN_SETTLE_US 600   // 91 % of the level: still clear of the release thresholds
#define EDGE_FLASH_SAMPLES       2

// Hysteresis and debounce (HANDOVER.md 6.2)
#define LOADED_PCT      90
#define RELEASED_PCT    40
#define DEBOUNCE_COUNT  4

// A switch to the white launcher needs readings above its loaded threshold for
// this long without a break: a bey pushed in too far on a black launcher lifts
// the pin past it for a moment before it drops back
#define LAUNCHER_WHITE_HOLD_US 3000000

// While empty, slowly track the baseline so drift does not cause false loads
#define BASELINE_TRACK_PCT  20
#define BASELINE_DIV        32  // EMA weight 1/32


static SemaphoreHandle_t s_lock;
static volatile load_led_hold_t s_hold;   // diag/meter: LED held on or off
static volatile uint32_t s_led_off_us;   // when the LED last went off (esp_timer, low 32 bits)

// Edge flash, shared between the task and the interrupts
static gptimer_handle_t s_timer;
static volatile bool s_edge_armed;      // the task wants edge flashes
static volatile bool s_edge_busy;       // a flash is in progress (LED on)
static volatile bool s_task_busy;       // a task-side reading owns the LED
static volatile uint32_t s_edge_interval_us;   // latest valid RPM edge interval, 0 unknown
static volatile uint32_t s_edge_deadline_us;   // the LED must be off by then
static volatile int s_edge_read_us;     // how long a reading takes, measured at the edge
static volatile int s_edge_off_raw;
static volatile uint32_t s_edge_seq;    // completed samples...
static volatile int s_edge_sample_off_raw;
static volatile int s_edge_sample_on_raw;
static uint32_t s_edge_taken_seq;       // ...and the last one the task took
static volatile uint32_t s_edge_late;   // flashes cut short (LED off before the reading)

static bool s_loaded;
static int s_streak;
static int s_last_pct;

static launcher_t s_launcher;   // black until a reading says otherwise
static int s_launcher_streak;     // readings in a row pointing to black
static bool s_white_pending;      // readings above the white threshold...
static int64_t s_white_since_us;  // ...without a break since then

static inline void IRAM_ATTR led_set(bool on)
{
    gpio_ll_set_level(&GPIO, PIN_LOAD_IR_LED, on ? 1 : 0);
}

static inline uint32_t IRAM_ATTR now_us(void)
{
    return (uint32_t)esp_timer_get_time();
}

// Average of a few raw conversions, from an interrupt; -1 on error
static int IRAM_ATTR read_raw_isr(void)
{
    int sum = 0;
    for (int i = 0; i < EDGE_FLASH_SAMPLES; i++) {
        int raw = sense_adc_read_raw_isr(ADC_CH_LOAD_SENSE);
        if (raw < 0) {
            return -1;
        }
        sum += raw;
    }
    return sum / EDGE_FLASH_SAMPLES;
}

// End of an edge flash: read with the LED on, then off at once
static bool IRAM_ATTR edge_flash_alarm(gptimer_handle_t timer, const gptimer_alarm_event_data_t *edata, void *ctx)
{
    if (!s_edge_busy) {
        return false;
    }
    if ((int32_t)(now_us() + s_edge_read_us - s_edge_deadline_us) > 0) {
        // Too late for a full reading before the deadline (another interrupt
        // held this one up): light off now, no sample
        led_set(false);
        s_edge_late++;
    } else {
        int on = read_raw_isr();
        led_set(false);
        if (on >= 0) {
            s_edge_sample_off_raw = s_edge_off_raw;
            s_edge_sample_on_raw = on;
            s_edge_seq++;
        }
    }
    s_led_off_us = now_us();
    s_edge_busy = false;
    return false;
}

void IRAM_ATTR load_on_rpm_edge(uint32_t edge_us)
{
    uint32_t interval = s_edge_interval_us;
    if (!s_edge_armed || s_edge_busy || s_task_busy || s_hold != LOAD_LED_AUTO || interval == 0) {
        return;
    }
    uint32_t t = now_us();
    if (t - s_led_off_us < FLASH_DARK_US) {
        return;   // the last flash's light hasn't faded yet: skip this edge
    }
    uint32_t deadline = edge_us + interval / 2 * EDGE_FLASH_WHITE_PCT / 100;

    int off = read_raw_isr();
    uint32_t t_on = now_us();
    int read_us = (int)(t_on - t);
    int settle = (int)(deadline - t_on) - read_us;
    if (off < 0 || settle < EDGE_FLASH_MIN_SETTLE_US) {
        return;   // not enough of the white part left for a reading
    }
    settle = settle < FLASH_SETTLE_US ? settle : FLASH_SETTLE_US;

    uint64_t count;
    gptimer_get_raw_count(s_timer, &count);
    gptimer_alarm_config_t alarm = {.alarm_count = count + (uint64_t)settle};
    s_edge_off_raw = off;
    s_edge_read_us = read_us;
    s_edge_deadline_us = deadline;
    s_edge_busy = true;
    led_set(true);
    gptimer_set_alarm_action(s_timer, &alarm);
}

void load_edge_flash_arm(bool on)
{
    if (on == s_edge_armed) {
        return;
    }
    if (on) {
        s_edge_taken_seq = s_edge_seq;   // nothing left over from before
        gptimer_enable(s_timer);
        gptimer_start(s_timer);
        s_edge_armed = true;
    } else {
        s_edge_armed = false;
        // Let a flash in progress finish (under 1 ms) before the timer stops
        while (s_edge_busy) {
            vTaskDelay(1);
        }
        gptimer_stop(s_timer);
        gptimer_disable(s_timer);
    }
}

void load_set_edge_interval(uint32_t interval_us)
{
    s_edge_interval_us = interval_us;
}

int load_take_edge_sample(int *on_mv, int *off_mv)
{
    uint32_t seq = s_edge_seq;
    if (seq == s_edge_taken_seq) {
        return LOAD_NO_SAMPLE;
    }
    // The interrupt may complete another sample while this reads; take the pair again then
    int on_raw, off_raw;
    do {
        seq = s_edge_seq;
        on_raw = s_edge_sample_on_raw;
        off_raw = s_edge_sample_off_raw;
    } while (seq != s_edge_seq);
    s_edge_taken_seq = seq;

    int on = sense_adc_raw_to_mv(ADC_CH_LOAD_SENSE, on_raw);
    int off = sense_adc_raw_to_mv(ADC_CH_LOAD_SENSE, off_raw);
    if (on_mv) {
        *on_mv = on;
    }
    if (off_mv) {
        *off_mv = off;
    }
    if (on < 0 || off < 0) {
        return INT_MIN;
    }
    return on - off;
}

uint32_t load_edge_flash_late_count(void)
{
    return s_edge_late;
}

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

    // 1 us ticks; only enabled while edge flashes are armed (the RPM channel
    // keeps the chip out of light sleep then anyway)
    gptimer_config_t timer_cfg = {
        .clk_src = GPTIMER_CLK_SRC_DEFAULT,
        .direction = GPTIMER_COUNT_UP,
        .resolution_hz = 1000000,
    };
    ESP_RETURN_ON_ERROR(gptimer_new_timer(&timer_cfg, &s_timer), TAG, "timer");
    gptimer_event_callbacks_t cbs = {.on_alarm = edge_flash_alarm};
    ESP_RETURN_ON_ERROR(gptimer_register_event_callbacks(s_timer, &cbs, NULL), TAG, "timer cb");
    return ESP_OK;
}

// Task-side readings take the LED from the edge flash: mark it as theirs (an
// edge then starts no flash) and wait for one in progress to end
static void take_led(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_task_busy = true;
    while (s_edge_busy) {
        esp_rom_delay_us(50);
    }
}

static void give_led(void)
{
    s_task_busy = false;
    xSemaphoreGive(s_lock);
}

int load_measure_raw(int samples, int *on_mv, int *off_mv)
{
    take_led();

    led_set(false);
    esp_rom_delay_us(LED_SETTLE_US);
    int off = sense_adc_read_mv(ADC_CH_LOAD_SENSE, samples);

    led_set(true);
    esp_rom_delay_us(LED_SETTLE_US);
    int on = sense_adc_read_mv(ADC_CH_LOAD_SENSE, samples);

    led_set(s_hold == LOAD_LED_ON);
    s_led_off_us = now_us();
    give_led();

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
    take_led();
    // Ambient first: the LED has been off since the last flash or poll. Flashes
    // are spaced so it normally has; right after a poll it may not have yet.
    uint32_t dark_us = now_us() - s_led_off_us;
    if (dark_us < FLASH_DARK_US) {
        esp_rom_delay_us(FLASH_DARK_US - dark_us);
    }
    int off = sense_adc_read_mv(ADC_CH_LOAD_SENSE, 2);
    led_set(true);
    esp_rom_delay_us(settle_us);
    int on = sense_adc_read_mv(ADC_CH_LOAD_SENSE, 2);
    led_set(s_hold == LOAD_LED_ON);
    s_led_off_us = now_us();
    give_led();

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
    load_edge_flash_arm(false);
    load_led_hold(LOAD_LED_AUTO);
}

void load_led_hold(load_led_hold_t hold)
{
    take_led();
    s_hold = hold;
    led_set(hold == LOAD_LED_ON);
    s_led_off_us = now_us();
    give_led();
}

load_led_hold_t load_led_held(void)
{
    return s_hold;
}

int load_read_raw_mv(int samples)
{
    take_led();
    int mv = sense_adc_read_mv(ADC_CH_LOAD_SENSE, samples);
    give_led();
    return mv;
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
    // With automatic white detection off ('cfg autowhite off'), always black.
    int pct;
    bool black_only = !settings_auto_white();
    launcher_t seen = s_launcher;
    if (black_only) {
        seen = LAUNCHER_BLACK;
    } else if (cal_percent(LAUNCHER_WHITE, diff_mv, &pct) && pct > LOADED_PCT) {
        seen = LAUNCHER_WHITE;
    } else if (cal_percent(LAUNCHER_BLACK, diff_mv, &pct) && pct < RELEASED_PCT) {
        seen = LAUNCHER_BLACK;
    }
    bool change = false;
    if (seen == s_launcher || !may_switch) {
        s_launcher_streak = 0;
        s_white_pending = false;
    } else if (black_only) {
        change = true;   // just switched off while on white: back to black at once
    } else if (seen == LAUNCHER_WHITE) {
        int64_t now = esp_timer_get_time();
        if (!s_white_pending) {
            s_white_pending = true;
            s_white_since_us = now;
        } else if (now - s_white_since_us >= LAUNCHER_WHITE_HOLD_US) {
            change = true;
        }
    } else if (++s_launcher_streak >= DEBOUNCE_COUNT) {
        change = true;
    }
    if (change) {
        s_launcher = seen;
        s_launcher_streak = 0;
        s_white_pending = false;
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
    if (!s_loaded && pct > -BASELINE_TRACK_PCT && pct < BASELINE_TRACK_PCT && s_launcher_streak == 0 &&
        !s_white_pending) {
        cfg->load_empty += (diff_mv - cfg->load_empty) / BASELINE_DIV;
    }
    return s_loaded;
}
