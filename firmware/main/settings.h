// Calibration and configuration persisted in NVS.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    uint16_t rpm_thr_mv;      // comparator threshold (RPM_REF), capped at RPM_THR_MAX_MV
    uint8_t edges_per_rev;    // rising edges per launcher revolution
    bool load_cal_valid;      // false = no load sensor calibration, RPM stays armed
    int16_t load_empty;       // ambient-cancelled LOAD_SENSE level, launcher empty (mV)
    int16_t load_loaded;      // same, with a top attached (mV)
} settings_t;

esp_err_t settings_init(void);

// Returns a pointer to the live settings. Writers must call settings_save().
settings_t *settings_get(void);
esp_err_t settings_save(void);
void settings_reset(void);
