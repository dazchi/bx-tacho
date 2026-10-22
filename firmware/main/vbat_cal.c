// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Darren Chi
#include "vbat_cal.h"

#include <math.h>
#include <stdlib.h>
#include "esp_check.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "vbat_cal";

#define PART        "fctry"
#define NAMESPACE   "vbat"
#define KEY         "cal"
#define BLOB_MAGIC  0x56424331u   // "VBC1"; change when vbat_cal_t changes

// Fit acceptance limits
#define MIN_SPAN_MV     500       // spread of the calibration points (VBAT)
// 470k/470k divider is nominally 2.0; 1% resistors plus ADC error stay within
// about +-4%, so +-5% also catches a wrong resistor fitted in production
#define GAIN_MIN_PPM    1900000
#define GAIN_MAX_PPM    2100000
#define OFFSET_MAX_MV   200
#define RESIDUAL_MAX_MV 15

typedef struct {
    uint32_t magic;
    vbat_cal_t cal;
} cal_blob_t;

static bool s_part_ok;
static esp_err_t s_init_err;      // why the partition could not be opened at boot
static bool s_valid;
static vbat_cal_t s_cal;

static int s_n;
static int s_pin[VBAT_CAL_MAX_POINTS];
static int s_vbat[VBAT_CAL_MAX_POINTS];

esp_err_t vbat_cal_init(void)
{
    // Never erase this partition, even if NVS reports a format problem: a
    // missing calibration falls back to nominal readings instead
    esp_err_t err = nvs_flash_init_partition(PART);
    if (err != ESP_OK) {
        s_init_err = err;
        ESP_LOGW(TAG, "factory partition unavailable (%s), VBAT uncalibrated", esp_err_to_name(err));
        return ESP_OK;
    }
    s_part_ok = true;

    nvs_handle_t h;
    if (nvs_open_from_partition(PART, NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "no VBAT calibration stored");
        return ESP_OK;
    }
    cal_blob_t blob;
    size_t len = sizeof(blob);
    err = nvs_get_blob(h, KEY, &blob, &len);
    nvs_close(h);
    if (err == ESP_OK && len == sizeof(blob) && blob.magic == BLOB_MAGIC) {
        s_cal = blob.cal;
        s_valid = true;
        ESP_LOGI(TAG, "VBAT calibration: gain %ld ppm, offset %ld mV, %u points",
                 (long)s_cal.gain_ppm, (long)s_cal.offset_mv, s_cal.points);
    } else {
        ESP_LOGW(TAG, "stored VBAT calibration missing or unreadable");
    }
    return ESP_OK;
}

bool vbat_cal_valid(void)
{
    return s_valid;
}

const vbat_cal_t *vbat_cal_get(void)
{
    return &s_cal;
}

int vbat_cal_nominal(int pin_mv)
{
    return pin_mv * 2;
}

int vbat_cal_apply(int pin_mv)
{
    if (!s_valid) {
        return vbat_cal_nominal(pin_mv);
    }
    return (int)(((int64_t)pin_mv * s_cal.gain_ppm + 500000) / 1000000) + s_cal.offset_mv;
}

void vbat_cal_session_begin(void)
{
    s_n = 0;
}

int vbat_cal_session_points(void)
{
    return s_n;
}

void vbat_cal_session_point(int i, int *pin_mv, int *vbat_mv)
{
    *pin_mv = s_pin[i];
    *vbat_mv = s_vbat[i];
}

esp_err_t vbat_cal_session_add(int pin_mv, int vbat_mv)
{
    if (s_n >= VBAT_CAL_MAX_POINTS) {
        return ESP_ERR_NO_MEM;
    }
    s_pin[s_n] = pin_mv;
    s_vbat[s_n] = vbat_mv;
    s_n++;
    return ESP_OK;
}

const char *vbat_cal_session_fit(vbat_cal_t *out, int residuals_mv[VBAT_CAL_MAX_POINTS])
{
    if (s_n < 2) {
        return "need at least 2 points";
    }
    int lo = s_vbat[0], hi = s_vbat[0];
    double sx = 0, sy = 0;
    for (int i = 0; i < s_n; i++) {
        lo = s_vbat[i] < lo ? s_vbat[i] : lo;
        hi = s_vbat[i] > hi ? s_vbat[i] : hi;
        sx += s_pin[i];
        sy += s_vbat[i];
    }
    if (hi - lo < MIN_SPAN_MV) {
        return "points must span at least 500 mV";
    }

    // Least squares: vbat = gain * pin + offset
    double mx = sx / s_n, my = sy / s_n, sxx = 0, sxy = 0;
    for (int i = 0; i < s_n; i++) {
        sxx += (s_pin[i] - mx) * (s_pin[i] - mx);
        sxy += (s_pin[i] - mx) * (s_vbat[i] - my);
    }
    if (sxx <= 0) {
        return "all points have the same ADC reading";
    }
    double gain = sxy / sxx;
    double offset = my - gain * mx;

    vbat_cal_t cal = {
        .gain_ppm = (int32_t)lround(gain * 1e6),
        .offset_mv = (int32_t)lround(offset),
        .points = (uint8_t)s_n,
        .min_mv = (int16_t)lo,
        .max_mv = (int16_t)hi,
    };
    if (cal.gain_ppm < GAIN_MIN_PPM || cal.gain_ppm > GAIN_MAX_PPM) {
        return "gain outside 1.9-2.1: check the divider (R3/R9 470k) and the entered values";
    }
    if (abs(cal.offset_mv) > OFFSET_MAX_MV) {
        return "offset larger than 200 mV: check the entered values";
    }

    // Residuals with the rounded coefficients, as they will be applied
    int max_err = 0;
    for (int i = 0; i < s_n; i++) {
        int pred = (int)(((int64_t)s_pin[i] * cal.gain_ppm + 500000) / 1000000) + cal.offset_mv;
        residuals_mv[i] = pred - s_vbat[i];
        max_err = abs(residuals_mv[i]) > max_err ? abs(residuals_mv[i]) : max_err;
    }
    cal.max_err_mv = (int16_t)max_err;
    *out = cal;
    if (max_err > RESIDUAL_MAX_MV) {
        return "a point is more than 15 mV off the fitted line: remeasure (check the entered values)";
    }
    return NULL;
}

esp_err_t vbat_cal_save(const vbat_cal_t *cal)
{
    // A new calibration is the one thing allowed to overwrite this partition: if
    // it held no valid NVS data at boot (e.g. leftovers on a fresh chip), format it
    if (!s_part_ok && (s_init_err == ESP_ERR_NVS_NO_FREE_PAGES || s_init_err == ESP_ERR_NVS_NEW_VERSION_FOUND)) {
        ESP_LOGW(TAG, "formatting the factory partition for the new calibration");
        ESP_RETURN_ON_ERROR(nvs_flash_erase_partition(PART), TAG, "erase");
        ESP_RETURN_ON_ERROR(nvs_flash_init_partition(PART), TAG, "init");
        s_part_ok = true;
    }
    ESP_RETURN_ON_FALSE(s_part_ok, ESP_ERR_INVALID_STATE, TAG, "factory partition unavailable");
    cal_blob_t blob = {.magic = BLOB_MAGIC, .cal = *cal};
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open_from_partition(PART, NAMESPACE, NVS_READWRITE, &h), TAG, "open");
    esp_err_t err = nvs_set_blob(h, KEY, &blob, sizeof(blob));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err == ESP_OK) {
        s_cal = *cal;
        s_valid = true;
    }
    return err;
}
