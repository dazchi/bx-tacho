// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Darren Chi
// Shared application state between the sensor task, the UI task and the console.
#pragma once

#include <stdbool.h>
#include "battery.h"
#include "launch.h"
#include "load.h"
#include "settings.h"

typedef enum {
    APP_MODE_RUN,     // normal launch measurement
    APP_MODE_DIAG,    // console drives the sensors directly
    APP_MODE_METER,   // continuous RPM on the display, for bench tests
    APP_MODE_VBATCAL, // production VBAT calibration: stays awake, no low-battery cutoff
} app_mode_t;

// Live readings shown in APP_MODE_VBATCAL
typedef struct {
    int pin_mv;           // VBAT_SENSE at the ADC pin
    int vbat_mv;          // VBAT with the stored calibration (nominal x2 if none)
} vbatcal_readings_t;

// Live sensor readings shown in APP_MODE_DIAG
typedef struct {
    load_led_hold_t led_hold;   // AUTO: the readings below; ON/OFF: load_mv is the raw LOAD_SENSE
    int load_mv;          // ambient-cancelled LOAD_SENSE (LED on - off)
    int load_on_mv;       // raw LOAD_SENSE with the LED on
    int load_off_mv;      // raw LOAD_SENSE with the LED off (ambient and stray light)
    int load_pct;         // position between the calibrated empty (0) and loaded (100) levels
    bool load_pct_valid;  // false without a usable load calibration
    bool rpm_on;          // RPM channel powered (RPM_SENSE only valid then)
    int rpm_mv;           // RPM_SENSE
    bool trig;            // comparator output (RPM_IR_TRIG)
} diag_readings_t;

typedef struct {
    launch_state_t state;
    int live_rpm;
    bool have_result;
    launch_result_t result;

    bool loaded;
    launcher_t launcher;      // black or white launcher, from the load reading
    bool load_cal_valid;      // ...and whether its calibration is usable
    int load_diff_mv;         // last ambient-cancelled LOAD_SENSE reading
    int load_pct;             // position between empty (0) and loaded (100)

    battery_status_t batt;
    app_mode_t mode;
    meter_stats_t meter;      // valid in APP_MODE_METER
    diag_readings_t diag;     // valid in APP_MODE_DIAG
    vbatcal_readings_t vbatcal;   // valid in APP_MODE_VBATCAL
    uint32_t idle_ms;         // time since the last user-visible activity
} app_snapshot_t;

void app_get_snapshot(app_snapshot_t *out);

// DIAG and METER pause the launch state machine. DIAG lets the console drive
// the sensors directly; METER keeps the RPM channel on and shows live RPM.
void app_set_mode(app_mode_t mode);
app_mode_t app_get_mode(void);
const char *app_mode_name(app_mode_t mode);

typedef enum {
    APP_SLEEP_IDLE,       // CONFIG_BXT_DEEP_SLEEP_S without activity
    APP_SLEEP_LOW_BATT,   // below CONFIG_BXT_CUTOFF_BATT_MV
    APP_SLEEP_COMMAND,    // console "sleep"
} app_sleep_reason_t;

// Countdown shown before a commanded deep sleep
#define APP_SLEEP_COUNTDOWN_S 5

void app_note_activity(void);

// Enter deep sleep (wake-up: USB plugged in, or the power switch).
// - Command: counts down APP_SLEEP_COUNTDOWN_S on the display.
// - Low battery: shows a dead-battery icon with the battery voltage, then a
//   short countdown (LOW_BATT_ICON_S / LOW_BATT_COUNTDOWN_S in main.c);
//   plugging in USB at any point cancels it so the battery can charge.
// - Idle timeout: no countdown (the display is already off).
// If USB is connected at the end, the sleep is cancelled and this returns false
// (waking is "USB present", so the chip would reboot at once). Otherwise it does
// not return.
bool app_enter_deep_sleep(app_sleep_reason_t reason);

// Automatic light sleep on battery (USB always keeps the chip awake)
void app_set_light_sleep(bool enable);
