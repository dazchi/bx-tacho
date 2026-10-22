// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Darren Chi
// Production VBAT calibration: VBAT = gain * pin_mv + offset, fitted from points
// measured against an external reference (power supply on J1 plus a multimeter).
// Stored in the "fctry" NVS partition, which `cfg reset` and NVS recovery never
// erase; only a recalibration or a full flash erase replaces it.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define VBAT_CAL_MAX_POINTS   8
// Usable range at J1: the board runs from the supply, so below ~3.2 V the 3.3 V
// regulator drops out; above ~4.4 V the ADC (VBAT / 2) nears its 2.5 V limit
#define VBAT_CAL_MIN_MV       3200
#define VBAT_CAL_MAX_MV       4400
#define VBAT_CAL_VERIFY_TOL_MV 10

typedef struct {
    int32_t gain_ppm;       // VBAT mV per pin mV, x1e6 (nominal 2000000: 470k/470k divider)
    int32_t offset_mv;
    uint8_t points;
    int16_t max_err_mv;     // largest fit residual
    int16_t min_mv;         // calibrated VBAT range
    int16_t max_mv;
} vbat_cal_t;

esp_err_t vbat_cal_init(void);
bool vbat_cal_valid(void);
const vbat_cal_t *vbat_cal_get(void);
// VBAT in mV from the ADC pin voltage; nominal x2 when not calibrated
int vbat_cal_apply(int pin_mv);
int vbat_cal_nominal(int pin_mv);

// Calibration session, kept in RAM until vbat_cal_save()
void vbat_cal_session_begin(void);
int vbat_cal_session_points(void);
void vbat_cal_session_point(int i, int *pin_mv, int *vbat_mv);
esp_err_t vbat_cal_session_add(int pin_mv, int vbat_mv);
// Fit the session points. Returns NULL and fills *out and residuals_mv[] (one per
// point) on success, otherwise a short reason why the result was rejected.
const char *vbat_cal_session_fit(vbat_cal_t *out, int residuals_mv[VBAT_CAL_MAX_POINTS]);

esp_err_t vbat_cal_save(const vbat_cal_t *cal);
