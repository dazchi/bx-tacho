// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Darren Chi
// BX Tacho: Beyblade X launcher RPM meter, ESP32-C3 firmware.
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include "app.h"
#include "bbp.h"
#include "ble_console.h"
#include "ble_ota.h"
#include "board.h"
#include "cli.h"
#include "display.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_ota_ops.h"
#include "esp_pm.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "driver/usb_serial_jtag.h"
#include "esp_timer.h"
#include "eventlog.h"
#include "freertos/FreeRTOS.h"
#include "icons.h"
#include "freertos/task.h"
#include "load.h"
#include "rpm.h"
#include "sense_adc.h"
#include "settings.h"
#include "vbat_cal.h"

static const char *TAG = "main";

#define LOAD_POLL_MS       50     // idle polling of the load sensor

// Load sampling while the RPM channel is armed (top loaded, launch). The load
// LED's light reflected off a loaded top reaches the RPM sensor. While the disc
// turns, load.c flashes it from the RPM interrupt at each rising edge, over
// before the white part of the disc ends, so the light can't make or move a
// rising edge (load_edge_flash_arm). While the disc is still, this task flashes
// it every LOAD_FLASH_IDLE_US, and rpm.c drops edges during the flash and
// LOAD_FLASH_TAIL_US after it (RPM_SENSE filter R15/C12, about 50 us): stopped
// on black, the disc could otherwise see the light as an edge.
#define LOAD_FLASH_IDLE_US    10000
#define LOAD_FLASH_TAIL_US    200
#define DISC_STILL_US         250000   // no edge for this long: the disc is still
#define IDLE_TICK_MS       20
#define BATTERY_PERIOD_MS  1000
#define UI_PERIOD_MS       100
#define UI_LIVE_PERIOD_MS  50
#define DIAG_SAMPLE_MS     100    // live sensor readings on the diag screen
#define VBATCAL_SAMPLE_MS  250    // live VBAT readings on the calibration screen
#define SPLASH_MS          1000   // boot screen
#define CREDIT_MS          1000   // ...then the credit

#define CONTRAST_NORMAL    0xCF
#define CONTRAST_DIM       0x10

// Low-battery shutdown: dead-battery icon, then a shorter countdown
#define LOW_BATT_ICON_S       2
#define LOW_BATT_COUNTDOWN_S  3

// Load-sensor gesture: BLE_GESTURE_COUNT loads in a row (top on, off, on, ...),
// each within BLE_GESTURE_GAP_MS of the one before, switch BLE on or off. The
// setting is applied by a restart, after BLE_GESTURE_SHOW_MS on the display.
#define BLE_GESTURE_COUNT     5
#define BLE_GESTURE_GAP_MS    1500
#define BLE_GESTURE_SHOW_MS   1500

static portMUX_TYPE s_snap_lock = portMUX_INITIALIZER_UNLOCKED;
static app_snapshot_t s_snap;
static volatile app_mode_t s_mode = APP_MODE_RUN;
static volatile bool s_mode_changed;
static volatile int64_t s_last_activity_us;
static esp_pm_lock_handle_t s_usb_pm_lock;
static esp_pm_lock_handle_t s_cal_pm_lock;    // no light sleep during VBAT calibration

// Deep-sleep hand-off. The UI task owns the display: it draws the countdown
// and switches the display off, so two tasks never drive the I2C bus at once.
static volatile bool s_show_batt_icon;            // low-battery icon on the display
static volatile int s_countdown;                  // >0: seconds left, shown on the display
static volatile app_sleep_reason_t s_countdown_reason;
static volatile bool s_sleep_pending;             // countdown or shutdown in progress
static volatile bool s_shutdown;                  // countdown over: other tasks leave the hardware alone
static volatile bool s_ui_parked;                 // UI task has switched the display off
static volatile int s_ble_gesture_msg;            // 1/2: BLE switched off/on, shown before the restart
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
    case APP_MODE_VBATCAL: return "vbat-cal";
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

    bbp_stop();
    rpm_power(false);
    load_led_off();

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

// Whether now is a safe moment to flash the load LED (see LOAD_FLASH_*)
// The disc turns: an RPM edge within DISC_STILL_US
static bool disc_turning(uint32_t now)
{
    uint32_t last, shortest;
    return rpm_recent_edges(&last, &shortest) > 0 && now - last <= DISC_STILL_US;
}

// Counts loads towards the BLE gesture; true when the gesture is complete
static bool ble_gesture_load(int64_t now_us)
{
    static int count;
    static int64_t last_us;
    if (count > 0 && now_us - last_us <= BLE_GESTURE_GAP_MS * 1000LL) {
        count++;
    } else {
        count = 1;
    }
    last_us = now_us;
    if (count < BLE_GESTURE_COUNT) {
        return false;
    }
    count = 0;
    return true;
}

// Flips the saved BLE setting and restarts to apply it (as the 'ble' command)
static void ble_gesture_apply(void)
{
    bool on = !settings_ble();
    settings_set_ble(on);
    eventlog_add(EV_BLE_GESTURE, on, 0);
    ESP_LOGI(TAG, "load gesture: BLE %s, restarting", on ? "on" : "off");
    s_ble_gesture_msg = on ? 2 : 1;
    bbp_stop();   // the app sees the disconnect at once
    vTaskDelay(pdMS_TO_TICKS(BLE_GESTURE_SHOW_MS));
    esp_restart();
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
    bool fast_load = false;      // flash sampling, waking on RPM edges
    uint32_t last_flash_us = 0;
    bool prev_usb = false;
    bool usb_event_pending = false;   // USB change not yet logged (waiting for a VBAT reading)
    int64_t usb_event_us = 0;
    bool prev_loaded = false;
    int raw_on_mv = 0;          // raw LOAD_SENSE readings of the latest sample, for the event log
    int raw_off_mv = 0;
    bool raw_from_flash = false;
    bool cal_lock_held = false;
    launch_state_t prev_state = launch_get_state();
    launcher_t prev_launcher = load_launcher();   // its RPM threshold is set at boot
    int64_t last_poll_us = 0;
    // First battery reading once VBAT has settled
    int64_t last_batt_us = (BATTERY_SETTLE_MS - BATTERY_PERIOD_MS) * 1000LL;

    for (;;) {
        if (s_shutdown) {
            // Another task is putting the chip to sleep; keep off the hardware
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        int64_t now = esp_timer_get_time();
        const launcher_cal_t *cfg = settings_cal(load_launcher());

        app_mode_t mode = s_mode;
        if (s_mode_changed) {
            s_mode_changed = false;
            eventlog_add(EV_MODE, mode, 0);
            // Hand the hardware over cleanly in either direction
            if (load_led_held() != LOAD_LED_AUTO) {
                load_led_off();
            }
            load_edge_flash_arm(false);
            fast_load = false;
            rpm_power(false);
            load_classifier_reset(false);
            launch_init();
            snap.loaded = prev_loaded = false;
            bbp_set_attached(false);
            snap.meter = (meter_stats_t) {0};
            snap.diag = (diag_readings_t) {0};
            snap.vbatcal = (vbatcal_readings_t) {0};
            last_poll_us = 0;
            if (mode == APP_MODE_METER) {
                rpm_power(true);
                launch_meter_start();
            }
            // Calibration runs from a supply on J1 with a data-only USB link (no
            // VBUS), so the firmware sees "battery": keep the USB console alive
            bool want_cal_lock = mode == APP_MODE_VBATCAL;
            if (want_cal_lock != cal_lock_held) {
                if (want_cal_lock) {
                    esp_pm_lock_acquire(s_cal_pm_lock);
                } else {
                    esp_pm_lock_release(s_cal_pm_lock);
                }
                cal_lock_held = want_cal_lock;
            }
        }

        if (mode == APP_MODE_METER) {
            launch_meter_update(&snap.meter);
            app_note_activity();   // keep the display on while metering
        } else if (mode == APP_MODE_VBATCAL) {
            if (now - last_poll_us >= VBATCAL_SAMPLE_MS * 1000LL) {
                last_poll_us = now;
                int pin = battery_read_pin_mv(16);
                snap.vbatcal.pin_mv = pin;
                snap.vbatcal.vbat_mv = pin >= 0 ? vbat_cal_apply(pin) : -1;
            }
            app_note_activity();   // keep the display on while calibrating
        } else if (mode == APP_MODE_DIAG) {
            // Only read the sensors; the console decides what is powered. The
            // percentage is computed directly so the classifier and its baseline
            // tracking stay untouched.
            if (now - last_poll_us >= DIAG_SAMPLE_MS * 1000LL) {
                last_poll_us = now;
                diag_readings_t *d = &snap.diag;
                // With the LED held on or off ('load led on|off', 'rpm scan') a
                // measurement would switch it: show the live raw voltage instead
                d->led_hold = load_led_held();
                if (d->led_hold == LOAD_LED_AUTO) {
                    d->load_mv = load_measure_raw(4, &d->load_on_mv, &d->load_off_mv);
                    load_track_launcher(d->load_mv, true);
                } else {
                    int mv = load_read_raw_mv(4);
                    d->load_mv = mv >= 0 ? mv : INT_MIN;
                }
                cfg = settings_cal(load_launcher());
                int span = cfg->load_loaded - cfg->load_empty;
                d->load_pct_valid = d->led_hold == LOAD_LED_AUTO && cfg->load_cal_valid && d->load_mv != INT_MIN &&
                                    abs(span) >= LOAD_MIN_CONTRAST_MV;
                d->load_pct = d->load_pct_valid ? (d->load_mv - cfg->load_empty) * 100 / span : 0;
                d->rpm_on = rpm_is_powered();
                d->rpm_mv = d->rpm_on ? rpm_read_sense_mv(4) : 0;
                d->trig = gpio_get_level(PIN_RPM_IR_TRIG) == 1;
            }
            app_note_activity();   // keep the display on while diagnosing
        } else if (mode == APP_MODE_RUN) {
            bool fast = snap.loaded || launch_get_state() == LAUNCH_PULLING;
            int diff = INT_MIN;
            bool sampled = false;

            if (fast && cfg->load_cal_valid) {
                fast_load = true;
                uint32_t t = (uint32_t)esp_timer_get_time();
                // Once the release is seen the launch needs no more load readings
                // until it ends: no flashes during the spin-down
                bool want = !launch_released();
                bool turning = want && disc_turning(t);
                load_edge_flash_arm(turning);
                if (turning) {
                    int d = load_take_edge_sample(&raw_on_mv, &raw_off_mv);
                    if (d != LOAD_NO_SAMPLE) {
                        diff = d;
                        raw_from_flash = true;
                        sampled = true;
                    }
                } else if (want && t - last_flash_us >= LOAD_FLASH_IDLE_US) {
                    last_flash_us = t;
                    rpm_mask_begin();
                    diff = load_flash_sample_raw(&raw_on_mv, &raw_off_mv);
                    rpm_mask_end(LOAD_FLASH_TAIL_US);
                    raw_from_flash = true;
                    sampled = true;
                }
            } else {
                load_edge_flash_arm(false);
                fast_load = false;
                if (now - last_poll_us >= LOAD_POLL_MS * 1000LL) {
                    last_poll_us = now;
                    diff = load_measure_raw(4, &raw_on_mv, &raw_off_mv);
                    raw_from_flash = false;
                    sampled = true;
                }
            }

            if (sampled) {
                snap.loaded = load_classify(diff, launch_get_state() != LAUNCH_PULLING);
                snap.load_diff_mv = diff;
                snap.load_pct = load_last_percent();
                cfg = settings_cal(load_launcher());
            }
            if (snap.loaded != prev_loaded) {
                prev_loaded = snap.loaded;
                // Raw values of the sample that completed the switch, and how it was taken
                eventlog_add_ext(snap.loaded ? EV_LOAD_ON : EV_LOAD_OFF, snap.load_diff_mv, snap.load_pct,
                                 raw_on_mv, raw_off_mv, raw_from_flash ? EV_FLAG_LOAD_FLASH : 0);
                app_note_activity();
                bbp_set_attached(snap.loaded);
                if (snap.loaded && launch_get_state() != LAUNCH_PULLING &&
                    ble_gesture_load(esp_timer_get_time())) {
                    ble_gesture_apply();   // does not return
                }
            }

            if (launch_update(snap.loaded, !cfg->load_cal_valid)) {
                app_note_activity();
            }
            // Sizes the next edge flash to the white part of the disc
            load_set_edge_interval(launch_edge_interval_us());
            snap.state = launch_get_state();
            snap.live_rpm = launch_live_rpm();
            snap.have_result = launch_get_result(&snap.result);
            if (snap.state != prev_state) {
                eventlog_add(EV_STATE, snap.state, prev_state);
                prev_state = snap.state;
                if (snap.state == LAUNCH_RESULT) {
                    uint32_t curve[LAUNCH_CURVE_MAX];
                    int n = launch_get_curve(curve, LAUNCH_CURVE_MAX);
                    bbp_record_shot(snap.result.peak_rpm, curve, n);
                }
            }
        }
        // Each launcher has its own RPM threshold (the load calibration is
        // looked up by load_classify())
        launcher_t launcher = load_launcher();
        if (launcher != prev_launcher) {
            prev_launcher = launcher;
            rpm_set_threshold_mv(cfg->rpm_thr_mv);
            eventlog_add(EV_LAUNCHER, launcher, mode == APP_MODE_DIAG ? snap.diag.load_mv : snap.load_diff_mv);
            // Loaded on the old launcher's settings and loaded on the new one's is
            // still a new load (an empty white launcher reads as loaded on the
            // black settings, until a top on it switches to white)
            launch_new_launcher();
        }
        snap.launcher = launcher;
        snap.load_cal_valid = cfg->load_cal_valid;
        snap.mode = mode;

        // USB presence is checked on every pass (one GPIO read), not with the battery:
        // the ESP32-C3's USB console does not survive light sleep, so the lock that
        // blocks it must be taken straight away, including right after boot
        bool usb = battery_usb_present();
        if (usb != prev_usb) {
            if (usb) {
                esp_pm_lock_acquire(s_usb_pm_lock);
            } else {
                esp_pm_lock_release(s_usb_pm_lock);
            }
            prev_usb = usb;
            usb_event_pending = true;
            usb_event_us = now;
            app_note_activity();
        }

        if (now - last_batt_us >= BATTERY_PERIOD_MS * 1000LL) {
            last_batt_us = now;
            battery_update(&snap.batt);
            // Battle Pass battery level: the charge current lifts the reading, so
            // while charging keep the last trusted value
            if (snap.batt.src == POWER_SRC_FULL) {
                bbp_set_battery_percent(100);
            } else if (snap.batt.vbat_valid) {
                bbp_set_battery_percent(snap.batt.percent);
            }

            // No cutoff while calibrating: the low calibration points are below it
            if (!usb && !s_sleep_pending && mode != APP_MODE_VBATCAL && snap.batt.vbat_valid &&
                snap.batt.vbat_mv < CONFIG_BXT_CUTOFF_BATT_MV) {
                ESP_LOGW(TAG, "battery %d mV below cutoff", snap.batt.vbat_mv);
                publish(&snap);
                app_enter_deep_sleep(APP_SLEEP_LOW_BATT);   // returns if USB is plugged in meanwhile
            }
        }

        // Log a USB change once there is a battery reading to go with it (at boot that
        // comes after VBAT has settled); give up waiting after one battery period
        if (usb_event_pending &&
            (snap.batt.vbat_mv > 0 || now - usb_event_us > BATTERY_PERIOD_MS * 1000LL)) {
            eventlog_add(prev_usb ? EV_USB_ON : EV_USB_OFF, snap.batt.vbat_mv, 0);
            usb_event_pending = false;
        }

        // Activity noted during this pass is newer than `now`; measure from the current
        // time so the idle time can't go negative (and wrap to a huge value)
        int64_t idle_us = esp_timer_get_time() - s_last_activity_us;
        snap.idle_ms = idle_us > 0 ? (uint32_t)(idle_us / 1000) : 0;
        publish(&snap);

        // LOADED doesn't count as busy: left loaded (or face down on a table, which
        // the load sensor also sees as loaded) it sleeps after the same idle timeout.
        // Loading and every state change are activity, so the timer starts afresh then.
        bool busy = snap.state == LAUNCH_PULLING || mode != APP_MODE_RUN;
        // A PC on a data-only cable (no VBUS) still counts as "USB": keep the console up
        bool usb_host = usb_serial_jtag_is_connected();
        if (!busy && !prev_usb && !usb_host && !s_sleep_pending &&
            snap.idle_ms > CONFIG_BXT_DEEP_SLEEP_S * 1000U) {
            app_enter_deep_sleep(APP_SLEEP_IDLE);
        }

        if (fast_load) {
            // Back right after the next RPM edge (to flash early in the interval), or in 1 ms
            ulTaskNotifyTake(pdTRUE, 1);
        } else {
            vTaskDelay(pdMS_TO_TICKS(IDLE_TICK_MS));
        }
    }
}

// BLE state icon, centred on the status line. The icons are odd widths with the
// rune in the middle column, so the rune stays put as the state changes. The
// longest labels ("VBAT CAL" left, "LOW xx%" + battery right) leave it room.
static void draw_ble_icon(int y)
{
    switch (bbp_link_state()) {
    case BBP_LINK_CONNECTED:
        display_bitmap((DISPLAY_W - ICON_BLE_CONNECTED_W) / 2, y, ICON_BLE_CONNECTED_W, ICON_BLE_H,
                       icon_ble_connected_9x9);
        break;
    case BBP_LINK_ADVERTISING:
        display_bitmap((DISPLAY_W - ICON_BLE_ON_W) / 2, y, ICON_BLE_ON_W, ICON_BLE_H, icon_ble_on_5x9);
        break;
    default:
        display_bitmap((DISPLAY_W - ICON_BLE_OFF_W) / 2, y, ICON_BLE_OFF_W, ICON_BLE_H, icon_ble_off_9x9);
        break;
    }
}

static void draw_status_line(const app_snapshot_t *s)
{
    const char *left;
    if (s->mode == APP_MODE_DIAG) {
        left = "DIAG";
    } else if (s->mode == APP_MODE_METER) {
        left = "METER";
    } else if (s->mode == APP_MODE_VBATCAL) {
        left = "VBAT CAL";
    } else if (s->state == LAUNCH_EMPTY && !s->load_cal_valid) {
        left = "ARMED";
    } else {
        static const char *names[] = {"READY", "LOADED", "LAUNCH!", "RESULT"};
        left = names[s->state];
    }
    // Icons span rows 0-8 and the text rows 1-7 (centred on them), above the rule at y=10
    const int icon_y = 0;
    const int text_y = icon_y + 1;
    display_text(0, text_y, left, 1);

    char right[12];
    const char *low = NULL;   // "LOW" ahead of the percentage, set below
    int right_end = DISPLAY_W;
    const int icon_x = DISPLAY_W - ICON_BATTERY_W;
    switch (s->batt.src) {
    case POWER_SRC_CHARGING:
        display_bitmap(icon_x, icon_y, ICON_BATTERY_W, ICON_BATTERY_H, icon_battery_charging_16x9);
        right_end = icon_x - 2;
        // The charge current lifts this a little, so it is only a rough guide
        if (s->batt.vbat_mv > 0) {
            int dv = (s->batt.vbat_mv + 50) / 100;   // decivolts, rounded
            snprintf(right, sizeof(right), "%d.%dV", dv / 10, dv % 10);
        } else {
            right[0] = '\0';
        }
        break;
    case POWER_SRC_FULL:
        display_bitmap(icon_x, icon_y, ICON_BATTERY_W, ICON_BATTERY_H, icon_battery_full_16x9);
        right_end = icon_x - 2;
        snprintf(right, sizeof(right), "FULL");
        break;
    default:
        if (!s->batt.vbat_valid) {
            right[0] = '\0';
            break;
        }
        if (s->batt.vbat_mv < CONFIG_BXT_LOW_BATT_MV) {
            low = "LOW";
        }
        snprintf(right, sizeof(right), "%d%%", s->batt.percent);
        // Level bar in 10 % steps, rounded up so any charge left shows a sliver
        display_bitmap(icon_x, icon_y, ICON_BATTERY_W, ICON_BATTERY_H, icon_battery_16x9);
        int fill_w = (s->batt.percent * ICON_BATTERY_FILL_W + 99) / 100;
        for (int y = 0; fill_w > 0 && y < ICON_BATTERY_FILL_H; y++) {
            display_hline(icon_x + ICON_BATTERY_FILL_X, icon_x + ICON_BATTERY_FILL_X + fill_w - 1,
                          icon_y + ICON_BATTERY_FILL_Y + y);
        }
        right_end = icon_x - 2;
        break;
    }
    int right_x = right_end - display_text_width(right, 1);
    display_text(right_x, text_y, right, 1);
    if (low != NULL) {
        // Closer than a space (7 px) so it keeps clear of the centred BLE icon
        display_text(right_x - 3 - display_text_width(low, 1), text_y, low, 1);
    }
    draw_ble_icon(icon_y);
    display_hline(0, DISPLAY_W - 1, icon_y + ICON_BATTERY_H + 1);
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

// A reading in double-size digits with a small "MV" after it
static void draw_value_mv(int x, int y, int mv)
{
    char num[12];
    snprintf(num, sizeof(num), "%d", mv);
    display_text(x, y, num, 2);
    display_text(x + display_text_width(num, 2) + 4, y + 7, "MV", 1);
}

// Diag mode: live LOAD_SENSE (difference large, raw LED on/off beside it) and RPM_SENSE
static void draw_diag_screen(const app_snapshot_t *s)
{
    const diag_readings_t *d = &s->diag;
    char line[24];

    display_clear();
    draw_status_line(s);

    display_text(0, 13, "LOAD_SENSE", 1);
    if (d->led_hold != LOAD_LED_AUTO) {
        // LED held: the live raw voltage, and how the LED is held
        const char *held = d->led_hold == LOAD_LED_ON ? "LED ON" : "LED OFF";
        display_text(DISPLAY_W - display_text_width(held, 1), 13, held, 1);
        if (d->load_mv == INT_MIN) {
            display_text(0, 22, "ERR", 2);
        } else {
            draw_value_mv(0, 22, d->load_mv);
        }
        display_text(DISPLAY_W - display_text_width("RAW", 1), 22, "RAW", 1);
    } else {
        // The launcher the reading says it's on, whose calibration gives the percentage
        if (d->load_pct_valid) {
            snprintf(line, sizeof(line), "%s %d%%", s->launcher == LAUNCHER_WHITE ? "WHITE" : "BLACK", d->load_pct);
        } else {
            snprintf(line, sizeof(line), "%s", s->launcher == LAUNCHER_WHITE ? "WHITE" : "BLACK");
        }
        display_text(DISPLAY_W - display_text_width(line, 1), 13, line, 1);
        if (d->load_mv == INT_MIN) {
            display_text(0, 22, "ERR", 2);
        } else {
            draw_value_mv(0, 22, d->load_mv);
        }
        snprintf(line, sizeof(line), "ON %d", d->load_on_mv);
        display_text(DISPLAY_W - display_text_width(line, 1), 22, line, 1);
        snprintf(line, sizeof(line), "OFF %d", d->load_off_mv);
        display_text(DISPLAY_W - display_text_width(line, 1), 30, line, 1);
    }

    display_text(0, 39, "RPM_SENSE", 1);
    snprintf(line, sizeof(line), "THR %d %s", rpm_get_threshold_mv(), d->trig ? "HI" : "LO");
    display_text(DISPLAY_W - display_text_width(line, 1), 39, line, 1);
    if (d->rpm_on) {
        draw_value_mv(0, 48, d->rpm_mv);
    } else {
        display_text(0, 48, "OFF", 2);
    }
    display_flush();
}

// VBAT calibration: VBAT (calibrated or nominal) large, the ADC pin voltage and
// the number of points captured so far
static void draw_vbatcal_screen(const app_snapshot_t *s)
{
    const vbatcal_readings_t *v = &s->vbatcal;
    char line[24];

    display_clear();
    draw_status_line(s);

    display_text(0, 13, "VBAT", 1);
    display_text(DISPLAY_W - display_text_width(vbat_cal_valid() ? "CAL" : "NOMINAL", 1), 13,
                 vbat_cal_valid() ? "CAL" : "NOMINAL", 1);
    if (v->vbat_mv < 0) {
        display_text(0, 22, "ERR", 2);
    } else {
        draw_value_mv(0, 22, v->vbat_mv);
    }

    display_text(0, 39, "ADC PIN", 1);
    snprintf(line, sizeof(line), "POINTS %d", vbat_cal_session_points());
    display_text(DISPLAY_W - display_text_width(line, 1), 39, line, 1);
    if (v->pin_mv < 0) {
        display_text(0, 48, "ERR", 2);
    } else {
        draw_value_mv(0, 48, v->pin_mv);
    }
    display_flush();
}

// "64:E8:33:A1:B2:C5", the Bluetooth address phones see, for the idle screen
static const char *ble_address_text(void)
{
    static char text[18];
    if (text[0] == '\0') {
        uint8_t mac[6];
        if (esp_read_mac(mac, ESP_MAC_BT) == ESP_OK) {
            snprintf(text, sizeof(text), "%02X:%02X:%02X:%02X:%02X:%02X",
                     mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        }
    }
    return text;
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
    if (s->mode == APP_MODE_DIAG) {
        draw_diag_screen(s);
        return;
    }
    if (s->mode == APP_MODE_VBATCAL) {
        draw_vbatcal_screen(s);
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
        // Nothing measured yet: room for the address, to find this one among
        // others in an app's device list
        if (bbp_link_state() != BBP_LINK_OFF) {
            note = ble_address_text();
        }
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

// Confirmation of the BLE gesture, shown until the restart
static void draw_ble_gesture(bool on)
{
    const char *big = on ? "BLE ON" : "BLE OFF";
    static const char *note = "RESTARTING";
    int w = on ? ICON_BLE_ON_W : ICON_BLE_OFF_W;

    display_clear();
    display_bitmap((DISPLAY_W - w) / 2, 6, w, ICON_BLE_H, on ? icon_ble_on_5x9 : icon_ble_off_9x9);
    display_text((DISPLAY_W - display_text_width(big, 2)) / 2, 22, big, 2);
    display_text((DISPLAY_W - display_text_width(note, 1)) / 2, 50, note, 1);
    display_flush();
}

// Firmware update over BLE: percent written, then RESTARTING
static void draw_update(int percent, bool restarting)
{
    static const char *title = "FIRMWARE UPDATE";
    char big[8];

    display_clear();
    display_text((DISPLAY_W - display_text_width(title, 1)) / 2, 0, title, 1);
    display_hline(0, DISPLAY_W - 1, 10);
    if (restarting) {
        display_text((DISPLAY_W - display_text_width("DONE", 3)) / 2, 20, "DONE", 3);
        display_text((DISPLAY_W - display_text_width("RESTARTING", 1)) / 2, 54, "RESTARTING", 1);
    } else {
        snprintf(big, sizeof(big), "%d%%", percent);
        display_text((DISPLAY_W - display_text_width(big, 3)) / 2, 16, big, 3);
        display_text((DISPLAY_W - display_text_width("DO NOT TURN OFF", 1)) / 2, 40, "DO NOT TURN OFF", 1);
        // Bar: a baseline across, filled above it as far as written
        int fill = percent * DISPLAY_W / 100;
        for (int y = 50; fill > 0 && y < 56; y++) {
            display_hline(0, fill - 1, y);
        }
        display_hline(0, DISPLAY_W - 1, 57);
    }
    display_flush();
}

// The console over BLE is locked: the code to type into the web app
static void draw_console_code(uint32_t value)
{
    static const char *title = "BLUETOOTH CONSOLE";
    static const char *hint = "ENTER THIS CODE";
    char code[12];

    snprintf(code, sizeof(code), "%03lu %03lu", (unsigned long)(value / 1000),
             (unsigned long)(value % 1000));
    display_clear();
    display_text((DISPLAY_W - display_text_width(title, 1)) / 2, 0, title, 1);
    display_hline(0, DISPLAY_W - 1, 10);
    display_text((DISPLAY_W - display_text_width(code, 3)) / 2, 22, code, 3);
    display_text((DISPLAY_W - display_text_width(hint, 1)) / 2, 56, hint, 1);
    display_flush();
}

// Boot screen: icon, project name and firmware version
static void draw_splash(void)
{
    char ver[40];

    snprintf(ver, sizeof(ver), "v%s", esp_app_get_description()->version);
    display_clear();
    // Logo lockup: icon on the left, name (two lines) and version on the right
    display_bitmap(6, (DISPLAY_H - ICON_BEYBLADE_H) / 2, ICON_BEYBLADE_W, ICON_BEYBLADE_H, icon_beyblade_40);
    display_text(54, 12, "BX", 2);
    display_text(54, 28, "TACHO", 2);
    display_text(54, 46, ver, 1);
    display_flush();
}

// Second boot screen: who made it
static void draw_credit(void)
{
    static const char *by = "MADE BY";
    static const char *name = "DAZCHI";
    static const char *url = "github.com/dazchi";

    display_clear();
    display_text((DISPLAY_W - display_text_width(by, 1)) / 2, 8, by, 1);
    display_text((DISPLAY_W - display_text_width(name, 2)) / 2, 22, name, 2);
    display_text((DISPLAY_W - display_text_width(url, 1)) / 2, 46, url, 1);
    display_flush();
}

static void ui_task(void *arg)
{
    app_snapshot_t s;
    bool on = true;
    bool dim = false;
    int64_t off_since_us = 0;

    draw_splash();
    vTaskDelay(pdMS_TO_TICKS(SPLASH_MS));
    draw_credit();
    vTaskDelay(pdMS_TO_TICKS(CREDIT_MS));

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

        int gesture = s_ble_gesture_msg;
        if (gesture) {
            if (!on) {
                display_power(true);
                on = true;
            }
            if (dim) {
                display_set_contrast(CONTRAST_NORMAL);
                dim = false;
            }
            draw_ble_gesture(gesture == 2);
            vTaskDelay(pdMS_TO_TICKS(UI_PERIOD_MS));
            continue;
        }

        int update_pct;
        bool restarting;
        if (ble_ota_progress(&update_pct, &restarting)) {
            if (!on) {
                display_power(true);
                on = true;
            }
            if (dim) {
                display_set_contrast(CONTRAST_NORMAL);
                dim = false;
            }
            draw_update(update_pct, restarting);
            vTaskDelay(pdMS_TO_TICKS(UI_PERIOD_MS));
            continue;
        }

        uint32_t code;
        if (ble_console_code(&code)) {
            if (!on) {
                display_power(true);
                on = true;
            }
            if (dim) {
                display_set_contrast(CONTRAST_NORMAL);
                dim = false;
            }
            draw_console_code(code);
            vTaskDelay(pdMS_TO_TICKS(UI_PERIOD_MS));
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

        bool live = s.state == LAUNCH_PULLING || s.mode != APP_MODE_RUN;
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
    ESP_ERROR_CHECK(vbat_cal_init());   // after settings_init(): the default NVS partition is set up
    if (bbp_init(settings_ble()) != ESP_OK) {
        ESP_LOGE(TAG, "Battle Pass emulation unavailable");   // the meter works without it
    }
    ESP_ERROR_CHECK(sense_adc_init());
    ESP_ERROR_CHECK(battery_init());
    ESP_ERROR_CHECK(rpm_init());
    ESP_ERROR_CHECK(load_init());
    rpm_set_threshold_mv(settings_cal(load_launcher())->rpm_thr_mv);   // until the first load reading
    launch_init();

    configure_pm(settings_light_sleep());
    ESP_ERROR_CHECK(esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "usb", &s_usb_pm_lock));
    ESP_ERROR_CHECK(esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "vbatcal", &s_cal_pm_lock));

    app_note_activity();

    if (display_init() == ESP_OK) {
        display_set_contrast(CONTRAST_NORMAL);
        display_set_flipped(settings_flip());
        s_ui_running = true;
        xTaskCreate(ui_task, "ui", 3072, NULL, 3, NULL);
    }
    TaskHandle_t sensors;
    xTaskCreate(sensor_task, "sensors", 4096, NULL, 5, &sensors);
    rpm_set_notify_task(sensors);
    rpm_set_edge_hook(load_on_rpm_edge);

    ESP_ERROR_CHECK(cli_start());

    // After an update over BLE the new firmware runs on probation: a reset
    // before this point boots the previous firmware again (bootloader rollback)
    esp_ota_img_states_t ota_state;
    if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &ota_state) == ESP_OK &&
        ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI(TAG, "new firmware started fine, update confirmed");
    }
    ESP_LOGI(TAG, "BX Tacho ready. Type 'help' for console commands.");
}
