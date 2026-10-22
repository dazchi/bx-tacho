// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Darren Chi
// Load-detect channel: KEY2 aimed at the launcher's rising center part,
// read through the ADC with ambient-light cancellation (HANDOVER.md 3.7, 6.2).
#pragma once

#include <stdbool.h>
#include "esp_err.h"

// Smallest |loaded - empty| difference accepted as a calibration (mV)
#define LOAD_MIN_CONTRAST_MV 30

esp_err_t load_init(void);

// One complete ambient-cancelled measurement: LED off, LED on, settle,
// difference averaged over `samples` reads. Restores the LED afterwards.
// Thread-safe. Returns mV, or INT_MIN on ADC error.
int load_measure(int samples);
// Same, also returning the raw LED-on and LED-off readings (mV, -1 on ADC error)
int load_measure_raw(int samples, int *on_mv, int *off_mv);

// Fast sample for while the RPM channel is armed: the LED is lit only for a short
// flash (about LOAD_FLASH_LEN_US), because its light reflected off a loaded top
// reaches the RPM sensor and can hide edges. Slightly less settled than
// load_measure_raw(). The caller masks RPM edges around it (rpm_mask_begin/end).
#define LOAD_FLASH_LEN_US 500   // upper bound of the time the LED is lit
int load_flash_sample_raw(int *on_mv, int *off_mv);

// Diag/meter: keep the LED on continuously (crosstalk tests). Cleared again by
// load_led_force(false) or load_led_off().
void load_led_force(bool on);
bool load_led_forced(void);
// LED off and any forcing cleared (deep sleep, mode changes)
void load_led_off(void);

// Debounced classifier. Feed it every measurement; returns the loaded state.
// Always false while the calibration is invalid.
bool load_classify(int diff_mv);
void load_classifier_reset(bool loaded);

// Position of the last sample between the empty (0) and loaded (100) levels
int load_last_percent(void);
