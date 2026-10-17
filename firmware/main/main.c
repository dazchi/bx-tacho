// BX Tacho: Beyblade X launcher RPM meter, ESP32-C3 firmware.
#include <limits.h>
#include <stdio.h>
#include "app.h"
#include "board.h"
#include "cli.h"
#include "display.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "load.h"
#include "rpm.h"
#include "sense_adc.h"
#include "settings.h"

static const char *TAG = "main";

#define LOAD_POLL_MS       50     // idle polling of the load sensor
#define IDLE_TICK_MS       20
#define BATTERY_PERIOD_MS  1000
#define UI_PERIOD_MS       100
#define UI_LIVE_PERIOD_MS  50

#define CONTRAST_NORMAL    0xCF
#define CONTRAST_DIM       0x10

static portMUX_TYPE s_snap_lock = portMUX_INITIALIZER_UNLOCKED;
static app_snapshot_t s_snap;
static volatile bool s_diag;
static volatile bool s_diag_changed;
static volatile int64_t s_last_activity_us;
static esp_pm_lock_handle_t s_usb_pm_lock;

static const gpio_num_t s_held_outputs[] = {
    PIN_LOAD_IR_LED, PIN_RPM_CMP_PWR, PIN_RPM_IR_LED, PIN_RPM_THR_PWM,
};

void app_get_snapshot(app_snapshot_t *out)
{
    taskENTER_CRITICAL(&s_snap_lock);
    *out = s_snap;
    taskEXIT_CRITICAL(&s_snap_lock);
}

void app_set_diag(bool on)
{
    s_diag = on;
    s_diag_changed = true;
}

bool app_is_diag(void)
{
    return s_diag;
}

void app_note_activity(void)
{
    s_last_activity_us = esp_timer_get_time();
}

void app_enter_deep_sleep(void)
{
    ESP_LOGI(TAG, "entering deep sleep, wake on USB plug-in or power switch");
    if (display_present()) {
        display_clear();
        display_text(40, 28, "SLEEP", 1);
        display_flush();
        vTaskDelay(pdMS_TO_TICKS(500));
        display_power(false);
    }

    rpm_power(false);
    load_hold_end();

    // Hold every output LOW so nothing back-feeds the unpowered RPM channel
    for (int i = 0; i < sizeof(s_held_outputs) / sizeof(s_held_outputs[0]); i++) {
        gpio_set_direction(s_held_outputs[i], GPIO_MODE_OUTPUT);
        gpio_set_level(s_held_outputs[i], 0);
        gpio_set_pull_mode(s_held_outputs[i], GPIO_FLOATING);
        gpio_hold_en(s_held_outputs[i]);
    }
    gpio_deep_sleep_hold_en();

    esp_sleep_enable_gpio_wakeup_on_hp_periph_powerdown(BIT64(PIN_VUSB_DET), ESP_GPIO_WAKEUP_GPIO_HIGH);
    esp_deep_sleep_start();
}

static void publish(const app_snapshot_t *snap)
{
    taskENTER_CRITICAL(&s_snap_lock);
    s_snap = *snap;
    taskEXIT_CRITICAL(&s_snap_lock);
}

// Sensors, launch state machine and power management
static void sensor_task(void *arg)
{
    app_snapshot_t snap = {0};
    bool hold = false;
    bool prev_usb = false;
    bool prev_loaded = false;
    int64_t last_poll_us = 0;
    int64_t last_batt_us = -BATTERY_PERIOD_MS * 1000LL;

    for (;;) {
        int64_t now = esp_timer_get_time();
        const settings_t *cfg = settings_get();

        if (s_diag_changed) {
            s_diag_changed = false;
            // Hand the hardware over cleanly in either direction
            if (hold) {
                load_hold_end();
                hold = false;
            }
            rpm_power(false);
            load_classifier_reset(false);
            launch_init();
            snap.loaded = prev_loaded = false;
        }

        if (!s_diag) {
            bool fast = snap.loaded || launch_get_state() == LAUNCH_PULLING;
            int diff = INT_MIN;
            bool sampled = false;

            if (fast && cfg->load_cal_valid) {
                if (!hold) {
                    load_hold_begin();
                    hold = true;
                }
                diff = load_hold_sample();
                sampled = true;
            } else {
                if (hold) {
                    load_hold_end();
                    hold = false;
                }
                if (now - last_poll_us >= LOAD_POLL_MS * 1000LL) {
                    last_poll_us = now;
                    diff = load_measure(4);
                    sampled = true;
                }
            }

            if (sampled) {
                snap.loaded = load_classify(diff);
                snap.load_diff_mv = diff;
                snap.load_pct = load_last_percent();
            }
            if (snap.loaded != prev_loaded) {
                prev_loaded = snap.loaded;
                app_note_activity();
            }

            if (launch_update(snap.loaded, !cfg->load_cal_valid)) {
                app_note_activity();
            }
            snap.state = launch_get_state();
            snap.live_rpm = launch_live_rpm();
            snap.have_result = launch_get_result(&snap.result);
        }
        snap.load_cal_valid = cfg->load_cal_valid;
        snap.diag = s_diag;

        if (now - last_batt_us >= BATTERY_PERIOD_MS * 1000LL) {
            last_batt_us = now;
            battery_update(&snap.batt);

            bool usb = snap.batt.src != POWER_SRC_BATTERY;
            if (usb != prev_usb) {
                // Keep the USB console alive while plugged in
                if (usb) {
                    esp_pm_lock_acquire(s_usb_pm_lock);
                } else {
                    esp_pm_lock_release(s_usb_pm_lock);
                }
                prev_usb = usb;
                app_note_activity();
            }

            if (!usb && snap.batt.vbat_valid && snap.batt.vbat_mv < CONFIG_BXT_CUTOFF_BATT_MV) {
                ESP_LOGW(TAG, "battery %d mV below cutoff", snap.batt.vbat_mv);
                app_enter_deep_sleep();
            }
        }

        snap.idle_ms = (uint32_t)((now - s_last_activity_us) / 1000);
        publish(&snap);

        bool busy = snap.state == LAUNCH_LOADED || snap.state == LAUNCH_PULLING || s_diag;
        if (!busy && !prev_usb && snap.idle_ms > CONFIG_BXT_DEEP_SLEEP_S * 1000U) {
            app_enter_deep_sleep();
        }

        vTaskDelay(hold ? 1 : pdMS_TO_TICKS(IDLE_TICK_MS));
    }
}

static void draw_status_line(const app_snapshot_t *s)
{
    const char *left;
    if (s->diag) {
        left = "DIAG";
    } else if (s->state == LAUNCH_EMPTY && !s->load_cal_valid) {
        left = "ARMED";
    } else {
        static const char *names[] = {"READY", "LOADED", "LAUNCH!", "RESULT"};
        left = names[s->state];
    }
    display_text(0, 0, left, 1);

    char right[12];
    switch (s->batt.src) {
    case POWER_SRC_CHARGING:
        snprintf(right, sizeof(right), "CHG");
        break;
    case POWER_SRC_FULL:
        snprintf(right, sizeof(right), "FULL");
        break;
    default:
        if (!s->batt.vbat_valid) {
            right[0] = '\0';
        } else if (s->batt.vbat_mv < CONFIG_BXT_LOW_BATT_MV) {
            snprintf(right, sizeof(right), "LOW %d%%", s->batt.percent);
        } else {
            snprintf(right, sizeof(right), "%d%%", s->batt.percent);
        }
        break;
    }
    display_text(DISPLAY_W - display_text_width(right, 1), 0, right, 1);
    display_hline(0, DISPLAY_W - 1, 10);
}

static void draw_screen(const app_snapshot_t *s)
{
    char big[8];
    const char *label;
    const char *note = "";

    if (s->state == LAUNCH_PULLING) {
        snprintf(big, sizeof(big), "%d", s->live_rpm);
        label = "RPM";
    } else if (s->have_result) {
        snprintf(big, sizeof(big), "%d", s->result.peak_rpm);
        label = "PEAK RPM";
        note = s->result.end == LAUNCH_END_SLOWDOWN ? "SLOW END" : "";
    } else {
        snprintf(big, sizeof(big), "----");
        label = "RPM";
    }

    display_clear();
    draw_status_line(s);
    display_text((DISPLAY_W - display_text_width(big, 4)) / 2, 18, big, 4);
    display_text(0, 56, label, 1);
    display_text(DISPLAY_W - display_text_width(note, 1), 56, note, 1);
    display_flush();
}

static void ui_task(void *arg)
{
    app_snapshot_t s;
    bool on = true;
    bool dim = false;

    for (;;) {
        app_get_snapshot(&s);

        bool want_on = s.idle_ms < CONFIG_BXT_DISPLAY_OFF_S * 1000U;
        bool want_dim = s.idle_ms >= CONFIG_BXT_DISPLAY_DIM_S * 1000U;
        if (want_on != on) {
            display_power(want_on);
            on = want_on;
        }
        if (on && want_dim != dim) {
            display_set_contrast(want_dim ? CONTRAST_DIM : CONTRAST_NORMAL);
            dim = want_dim;
        }
        if (on) {
            draw_screen(&s);
        }

        vTaskDelay(pdMS_TO_TICKS(s.state == LAUNCH_PULLING ? UI_LIVE_PERIOD_MS : UI_PERIOD_MS));
    }
}

void app_main(void)
{
    // Release pins held through deep sleep before reconfiguring them
    for (int i = 0; i < sizeof(s_held_outputs) / sizeof(s_held_outputs[0]); i++) {
        gpio_hold_dis(s_held_outputs[i]);
    }

    ESP_ERROR_CHECK(settings_init());
    ESP_ERROR_CHECK(sense_adc_init());
    ESP_ERROR_CHECK(battery_init());
    ESP_ERROR_CHECK(rpm_init());
    ESP_ERROR_CHECK(load_init());
    rpm_set_threshold_mv(settings_get()->rpm_thr_mv);
    launch_init();

    esp_pm_config_t pm = {
        .max_freq_mhz = 160,
        .min_freq_mhz = 40,
        .light_sleep_enable = true,
    };
    ESP_ERROR_CHECK(esp_pm_configure(&pm));
    ESP_ERROR_CHECK(esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "usb", &s_usb_pm_lock));

    app_note_activity();

    if (display_init() == ESP_OK) {
        display_set_contrast(CONTRAST_NORMAL);
        xTaskCreate(ui_task, "ui", 3072, NULL, 3, NULL);
    }
    xTaskCreate(sensor_task, "sensors", 4096, NULL, 5, NULL);

    ESP_ERROR_CHECK(cli_start());
    ESP_LOGI(TAG, "BX Tacho ready. Type 'help' for console commands.");
}
