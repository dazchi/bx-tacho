// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Darren Chi
// Calibration and configuration persisted in NVS.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

// Launchers with a dark housing reflect little of the load LED's light, white
// ones a lot: each kind gets its own calibration, and the load readings tell
// which one BX Tacho is on (load_track_launcher())
typedef enum {
    LAUNCHER_BLACK,
    LAUNCHER_WHITE,
    LAUNCHER_COUNT,
} launcher_t;

typedef struct {
    uint16_t rpm_thr_mv;      // comparator threshold (RPM_REF), capped at RPM_THR_MAX_MV
    bool load_cal_valid;      // false = no load sensor calibration, RPM stays armed
    int16_t load_empty;       // ambient-cancelled LOAD_SENSE level, launcher empty (mV)
    int16_t load_loaded;      // same, with a top attached (mV)
} launcher_cal_t;

typedef struct {
    uint8_t edges_per_rev;    // rising edges per launcher revolution
    launcher_cal_t cal[LAUNCHER_COUNT];
} settings_t;

esp_err_t settings_init(void);

// Returns a pointer to the live settings. Writers must call settings_save().
settings_t *settings_get(void);
// The calibration of one kind of launcher, in the live settings
launcher_cal_t *settings_cal(launcher_t launcher);
const char *launcher_name(launcher_t launcher);   // "black", "white"
esp_err_t settings_save(void);
void settings_reset(void);

// Automatic light sleep on battery (default on); saved immediately
bool settings_light_sleep(void);
esp_err_t settings_set_light_sleep(bool enable);

// Battle Pass emulation over BLE (default on); saved immediately, applied at boot
bool settings_ble(void);
esp_err_t settings_set_ble(bool enable);

// Display turned 180 degrees, for holding the launcher in the other hand
// (default off); saved immediately
bool settings_flip(void);
esp_err_t settings_set_flip(bool flip);

// Automatic white launcher detection (default on); off: always the black
// launcher's settings. Saved immediately.
bool settings_auto_white(void);
esp_err_t settings_set_auto_white(bool on);

// Optional Battle Pass extension in use, in builds that have one (bbp_ext.h;
// default off); saved immediately, applied at boot
bool settings_bbp_ext(void);
esp_err_t settings_set_bbp_ext(bool on);
