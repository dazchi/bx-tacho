// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Darren Chi
// Shared ADC1 one-shot unit for VBAT_SENSE, RPM_SENSE and LOAD_SENSE.
#pragma once

#include "esp_err.h"
#include "hal/adc_types.h"

esp_err_t sense_adc_init(void);

// Calibrated reading in mV, averaged over `samples` conversions.
// Thread-safe. Returns -1 on error.
int sense_adc_read_mv(adc_channel_t ch, int samples);

// One raw conversion from an interrupt (IRAM; CONFIG_ADC_ONESHOT_CTRL_FUNC_IN_IRAM).
// Each conversion runs in the driver's critical section, so it can't land in
// the middle of a task's reading. Returns -1 on error.
int sense_adc_read_raw_isr(adc_channel_t ch);
// Calibrated mV for a raw reading, -1 on error
int sense_adc_raw_to_mv(adc_channel_t ch, int raw);
