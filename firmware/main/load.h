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

// Fast mode for while a top is loaded: measure ambient once, then keep the
// LED on so each sample is a single ADC read.
void load_hold_begin(void);
int load_hold_sample(void);
void load_hold_end(void);

// Debounced classifier. Feed it every measurement; returns the loaded state.
// Always false while the calibration is invalid.
bool load_classify(int diff_mv);
void load_classifier_reset(bool loaded);

// Position of the last sample between the empty (0) and loaded (100) levels
int load_last_percent(void);
