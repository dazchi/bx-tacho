// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Darren Chi
// Load-detect channel: KEY2 aimed at the launcher's rising center part,
// read through the ADC with ambient-light cancellation (HANDOVER.md 3.7, 6.2).
#pragma once

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "settings.h"

// Smallest |loaded - empty| difference accepted as a calibration (mV)
#define LOAD_MIN_CONTRAST_MV 30

esp_err_t load_init(void);

// One complete ambient-cancelled measurement: LED off, LED on, settle,
// difference averaged over `samples` reads. Restores the LED afterwards.
// Thread-safe. Returns mV, or INT_MIN on ADC error.
int load_measure(int samples);
// Same, also returning the raw LED-on and LED-off readings (mV, -1 on ADC error)
int load_measure_raw(int samples, int *on_mv, int *off_mv);

// Fast sample for while the RPM channel is armed and the disc is still: the LED
// is lit only for a short flash (under 1 ms), because its light reflected off a
// loaded top reaches the RPM sensor and can make edges. The caller masks RPM
// edges around it (rpm_mask_begin/end).
int load_flash_sample_raw(int *on_mv, int *off_mv);
// Same with another settling time (console tests)
int load_flash_sample_settle_raw(int settle_us, int *on_mv, int *off_mv);

// While the disc turns: a flash at each rising RPM edge (from that interrupt),
// over before the white part of the disc ends, so its light can't make or move
// a rising edge. load_on_rpm_edge() is the RPM interrupt's hook; the sensor task
// arms it, keeps the latest valid edge interval up to date (0: unknown, no
// flashes) and takes the samples.
void load_edge_flash_arm(bool on);
void load_set_edge_interval(uint32_t interval_us);
void load_on_rpm_edge(uint32_t edge_us);
// The newest sample since the last call: ambient-cancelled mV and the raw
// readings, INT_MIN on ADC error, LOAD_NO_SAMPLE if there is none
#define LOAD_NO_SAMPLE (INT_MIN + 1)
int load_take_edge_sample(int *on_mv, int *off_mv);
// Edge flashes cut short (another interrupt delayed the reading past the deadline)
uint32_t load_edge_flash_late_count(void);

// Diag/meter: hold the LED on (crosstalk tests) or off (no measurement flashes:
// the sensor sees only ambient light), or AUTO, where measurements light it as
// they need. Back to AUTO with load_led_off() too.
typedef enum {
    LOAD_LED_AUTO,
    LOAD_LED_ON,
    LOAD_LED_OFF,
} load_led_hold_t;
void load_led_hold(load_led_hold_t hold);
load_led_hold_t load_led_held(void);
// The live LOAD_SENSE voltage, leaving the LED as it is (while it is held).
// mV, -1 on ADC error.
int load_read_raw_mv(int samples);
// LED off, back to AUTO and edge flashes off (deep sleep, mode changes)
void load_led_off(void);

// Debounced classifier. Feed it every measurement; returns the loaded state,
// using the calibration of the launcher it's on (load_track_launcher()).
// Always false while that calibration is invalid. `may_switch` false keeps
// the launcher as it is (during a launch).
bool load_classify(int diff_mv, bool may_switch);
void load_classifier_reset(bool loaded);

// Which launcher BX Tacho is on: black from boot; white once a reading is above
// the white launcher's loaded threshold, black again once one is below the
// black launcher's released threshold (white: held for 3 s without a break,
// as the pin pushed too far on a black launcher crosses it briefly; black: a
// few readings in a row).
// Called by load_classify(); diag mode calls it directly. Returns the launcher.
launcher_t load_track_launcher(int diff_mv, bool may_switch);
launcher_t load_launcher(void);

// Position of the last sample between the empty (0) and loaded (100) levels
int load_last_percent(void);
