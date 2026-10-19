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
#include "eventlog.h"
#include "freertos/FreeRTOS.h"
#include "icons.h"
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

// Low-battery shutdown: dead-battery icon, then a shorter countdown
#define LOW_BATT_ICON_S       2
#define LOW_BATT_COUNTDOWN_S  3

static portMUX_TYPE s_snap_lock = portMUX_INITIALIZER_UNLOCKED;
static app_snapshot_t s_snap;
static volatile app_mode_t s_mode = APP_MODE_RUN;
static volatile bool s_mode_changed;
static volatile int64_t s_last_activity_us;
static esp_pm_lock_handle_t s_usb_pm_lock;

// Deep-sleep hand-off. The UI task owns the display: it draws the countdown
// and switches the display off, so two tasks never drive the I2C bus at once.
static volatile bool s_show_batt_icon;            // low-battery icon on the display
static volatile int s_countdown;                  // >0: seconds left, shown on the display
static volatile app_sleep_reason_t s_countdown_reason;
static volatile bool s_sleep_pending;             // countdown or shutdown in progress
static volatile bool s_shutdown;                  // countdown over: other tasks leave the hardware alone
static volatile bool s_ui_parked;                 // UI task has switched the display off
static bool s_ui_running;

static const gpio_num_t s_held_outputs[] = {
    PIN_LOAD_IR_LED, PIN_RPM_CMP_PWR, PIN_RPM_IR_LED, PIN_RPM_THR_PWM,
};

void app_get_snapshot(app_snapshot_t *out)
{
    taskENTER_CRITICAL(&s_snap_lock);
    *out = s_snap;
    taskEXIT_CRITICAL(&s_snap_lock);
}

void app_set_mode(app_mode_t mode)
{
    s_mode = mode;
    s_mode_changed = true;
}

app_mode_t app_get_mode(void)
{
    return s_mode;
}

const char *app_mode_name(app_mode_t mode)
{
    switch (mode) {
    case APP_MODE_DIAG:  return "diag";
    case APP_MODE_METER: return "meter";
    default:             return "run";
    }
}

void app_note_activity(void)
{
    s_last_activity_us = esp_timer_get_time();
}

static void configure_pm(bool light_sleep)
{
    esp_pm_config_t pm = {
        .max_freq_mhz = 160,
        .min_freq_mhz = 40,
        .light_sleep_enable = light_sleep,
    };
    ESP_ERROR_CHECK(esp_pm_configure(&pm));
}

void app_set_light_sleep(bool enable)
{
    configure_pm(enable);
}

bool app_enter_deep_sleep(app_sleep_reason_t reason)
{
    if (s_sleep_pending) {
        return false;   // already on its way (e.g. command and low battery at once)
    }
    s_sleep_pending = true;

    // The idle timeout skips the countdown: the display is already off by then
    if (reason != APP_SLEEP_IDLE) {
        // On low battery, plugging in USB at any point means it can charge instead
        bool low_batt = reason == APP_SLEEP_LOW_BATT;
        s_countdown_reason = reason;
        if (low_batt) {
            s_show_batt_icon = true;
            for (int i = 0; i < LOW_BATT_ICON_S && !battery_usb_present(); i++) {
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
            s_show_batt_icon = false;
        }
        int seconds = low_batt ? LOW_BATT_COUNTDOWN_S : APP_SLEEP_COUNTDOWN_S;
        for (int i = seconds; i > 0 && !(low_batt && battery_usb_present()); i--) {
            s_countdown = i;
            ESP_LOGI(TAG, "deep sleep in %d s%s", i, battery_usb_present() ? " - unplug USB now" : "");
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
        if (battery_usb_present()) {
            ESP_LOGW(TAG, "USB connected, deep sleep cancelled");
            s_countdown = 0;
            s_sleep_pending = false;
            app_note_activity();
            return false;
        }
    }

    s_shutdown = true;
    s_countdown = 0;
    for (int i = 0; s_ui_running && !s_ui_parked && i < 50; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    app_snapshot_t s;
    app_get_snapshot(&s);
    eventlog_add(EV_DEEP_SLEEP, reason, s.batt.vbat_mv);
    ESP_LOGI(TAG, "entering deep sleep, wake on USB plug-in or power switch");

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

    // Automatic light sleep leaves a timer wake-up armed for the next tick; without
    // clearing it, deep sleep ends within a second and the chip reboots.
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
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
    launch_state_t prev_state = launch_get_state();
    int64_t last_poll_us = 0;
    int64_t last_batt_us = -BATTERY_PERIOD_MS * 1000LL;

    for (;;) {
        if (s_shutdown) {
            // Another task is putting the chip to sleep; keep off the hardware
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        int64_t now = esp_timer_get_time();
        const settings_t *cfg = settings_get();

        app_mode_t mode = s_mode;
        if (s_mode_changed) {
            s_mode_changed = false;
            eventlog_add(EV_MODE, mode, 0);
            // Hand the hardware over cleanly in either direction
            if (hold) {
                load_hold_end();
                hold = false;
            }
            rpm_power(false);
            load_classifier_reset(false);
            launch_init();
            snap.loaded = prev_loaded = false;
            snap.meter = (meter_stats_t) {0};
            if (mode == APP_MODE_METER) {
                rpm_power(true);
                launch_meter_start();
            }
        }

        if (mode == APP_MODE_METER) {
            launch_meter_update(&snap.meter);
            app_note_activity();   // keep the display on while metering
        } else if (mode == APP_MODE_RUN) {
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
                eventlog_add(snap.loaded ? EV_LOAD_ON : EV_LOAD_OFF, snap.load_diff_mv, snap.load_pct);
                app_note_activity();
            }

            if (launch_update(snap.loaded, !cfg->load_cal_valid)) {
                app_note_activity();
            }
            snap.state = launch_get_state();
            if (snap.state != prev_state) {
                eventlog_add(EV_STATE, snap.state, prev_state);
                prev_state = snap.state;
            }
            snap.live_rpm = launch_live_rpm();
            snap.have_result = launch_get_result(&snap.result);
        }
        snap.load_cal_valid = cfg->load_cal_valid;
        snap.mode = mode;

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
                eventlog_add(usb ? EV_USB_ON : EV_USB_OFF, snap.batt.vbat_mv, 0);
                app_note_activity();
            }

            if (!usb && !s_sleep_pending && snap.batt.vbat_valid &&
                snap.batt.vbat_mv < CONFIG_BXT_CUTOFF_BATT_MV) {
                ESP_LOGW(TAG, "battery %d mV below cutoff", snap.batt.vbat_mv);
                publish(&snap);
                app_enter_deep_sleep(APP_SLEEP_LOW_BATT);   // returns if USB is plugged in meanwhile
            }
        }

        // Activity noted during this pass is newer than `now`; measure from the current
        // time so the idle time can't go negative (and wrap to a huge value)
        int64_t idle_us = esp_timer_get_time() - s_last_activity_us;
        snap.idle_ms = idle_us > 0 ? (uint32_t)(idle_us / 1000) : 0;
        publish(&snap);

        bool busy = snap.state == LAUNCH_LOADED || snap.state == LAUNCH_PULLING || mode != APP_MODE_RUN;
        if (!busy && !prev_usb && !s_sleep_pending && snap.idle_ms > CONFIG_BXT_DEEP_SLEEP_S * 1000U) {
            app_enter_deep_sleep(APP_SLEEP_IDLE);
        }

        vTaskDelay(hold ? 1 : pdMS_TO_TICKS(IDLE_TICK_MS));
    }
}

static void draw_status_line(const app_snapshot_t *s)
{
    const char *left;
    if (s->mode == APP_MODE_DIAG) {
        left = "DIAG";
    } else if (s->mode == APP_MODE_METER) {
        left = "METER";
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

// Meter mode: last revolution large, then the 1 s average, spread and filter counters
static void draw_meter_screen(const app_snapshot_t *s)
{
    const meter_stats_t *m = &s->meter;
    char big[8];
    char line[24];

    display_clear();
    draw_status_line(s);
    // 0 once the disc has stopped, rather than the last revolution before it did
    snprintf(big, sizeof(big), "%d", m->avg_rpm ? m->inst_rpm : 0);
    display_text((DISPLAY_W - display_text_width(big, 4)) / 2, 14, big, 4);

    snprintf(line, sizeof(line), "MIN %d MAX %d", m->min_rpm, m->max_rpm);
    display_text(0, 46, line, 1);
    snprintf(line, sizeof(line), "AVG %d", m->avg_rpm);
    display_text(0, 56, line, 1);
    snprintf(line, sizeof(line), "REJ %d/%d", m->rejected_max_rpm, m->rejected_relative);
    display_text(DISPLAY_W - display_text_width(line, 1), 56, line, 1);
    display_flush();
}

static void draw_screen(const app_snapshot_t *s)
{
    char big[8];
    const char *label;
    const char *note = "";

    if (s->mode == APP_MODE_METER) {
        draw_meter_screen(s);
        return;
    }

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

static void draw_sleep_countdown(int seconds, app_sleep_reason_t reason)
{
    static const char *title = "DEEP SLEEP IN";
    const char *why = reason == APP_SLEEP_LOW_BATT ? "LOW BATTERY" : "SLEEP COMMAND";
    const char *hint = battery_usb_present() ? "UNPLUG USB NOW" : "PLUG IN USB TO WAKE";
    char big[4];

    display_clear();
    display_text((DISPLAY_W - display_text_width(title, 1)) / 2, 0, title, 1);
    display_hline(0, DISPLAY_W - 1, 10);
    snprintf(big, sizeof(big), "%d", seconds);
    display_text((DISPLAY_W - display_text_width(big, 4)) / 2, 15, big, 4);
    display_text((DISPLAY_W - display_text_width(why, 1)) / 2, 46, why, 1);
    display_text((DISPLAY_W - display_text_width(hint, 1)) / 2, 56, hint, 1);
    display_flush();
}

static void draw_low_battery(int vbat_mv)
{
    static const char *text = "LOW BATTERY";
    char volts[24];
    int cv = (vbat_mv + 5) / 10;   // centivolts, rounded

    snprintf(volts, sizeof(volts), "%d.%02d V", cv / 100, cv % 100);
    display_clear();
    display_bitmap((DISPLAY_W - ICON_BATTERY_OFF_W) / 2, 0, ICON_BATTERY_OFF_W, ICON_BATTERY_OFF_H,
                   icon_battery_off_48);
    display_text((DISPLAY_W - display_text_width(text, 1)) / 2, 47, text, 1);
    display_text((DISPLAY_W - display_text_width(volts, 1)) / 2, 56, volts, 1);
    display_flush();
}

static void ui_task(void *arg)
{
    app_snapshot_t s;
    bool on = true;
    bool dim = false;
    int64_t off_since_us = 0;

    for (;;) {
        if (s_shutdown) {
            if (on) {
                display_power(false);
                on = false;
            }
            s_ui_parked = true;
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        int countdown = s_countdown;
        bool batt_icon = s_show_batt_icon;
        if (batt_icon || countdown > 0) {
            if (!on) {
                display_power(true);
                on = true;
            }
            if (dim) {
                display_set_contrast(CONTRAST_NORMAL);
                dim = false;
            }
            if (batt_icon) {
                app_get_snapshot(&s);
                draw_low_battery(s.batt.vbat_mv);
            } else {
                draw_sleep_countdown(countdown, s_countdown_reason);
            }
            vTaskDelay(pdMS_TO_TICKS(UI_PERIOD_MS));
            continue;
        }

        app_get_snapshot(&s);

        bool want_on = s.idle_ms < CONFIG_BXT_DISPLAY_OFF_S * 1000U;
        bool want_dim = s.idle_ms >= CONFIG_BXT_DISPLAY_DIM_S * 1000U;
        if (want_on != on) {
            display_power(want_on);
            on = want_on;
            if (on) {
                eventlog_add(EV_DISPLAY_ON, (int)((esp_timer_get_time() - off_since_us) / 1000000), 0);
            } else {
                off_since_us = esp_timer_get_time();
                eventlog_add(EV_DISPLAY_OFF, 0, 0);
            }
        }
        if (on && want_dim != dim) {
            display_set_contrast(want_dim ? CONTRAST_DIM : CONTRAST_NORMAL);
            dim = want_dim;
        }
        if (on) {
            draw_screen(&s);
        }

        bool live = s.state == LAUNCH_PULLING || s.mode == APP_MODE_METER;
        vTaskDelay(pdMS_TO_TICKS(live ? UI_LIVE_PERIOD_MS : UI_PERIOD_MS));
    }
}

void app_main(void)
{
    // Release pins held through deep sleep before reconfiguring them
    for (int i = 0; i < sizeof(s_held_outputs) / sizeof(s_held_outputs[0]); i++) {
        gpio_hold_dis(s_held_outputs[i]);
    }

    eventlog_init();
    ESP_ERROR_CHECK(settings_init());
    ESP_ERROR_CHECK(sense_adc_init());
    ESP_ERROR_CHECK(battery_init());
    ESP_ERROR_CHECK(rpm_init());
    ESP_ERROR_CHECK(load_init());
    rpm_set_threshold_mv(settings_get()->rpm_thr_mv);
    launch_init();

    configure_pm(settings_light_sleep());
    ESP_ERROR_CHECK(esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "usb", &s_usb_pm_lock));

    app_note_activity();

    if (display_init() == ESP_OK) {
        display_set_contrast(CONTRAST_NORMAL);
        s_ui_running = true;
        xTaskCreate(ui_task, "ui", 3072, NULL, 3, NULL);
    }
    xTaskCreate(sensor_task, "sensors", 4096, NULL, 5, NULL);

    ESP_ERROR_CHECK(cli_start());
    ESP_LOGI(TAG, "BX Tacho ready. Type 'help' for console commands.");
}
