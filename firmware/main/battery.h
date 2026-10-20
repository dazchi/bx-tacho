// Battery voltage, USB presence and charge state (HANDOVER.md 6.4).
#pragma once

#include <stdbool.h>
#include "esp_err.h"

typedef enum {
    POWER_SRC_BATTERY,    // VUSB_DET LOW
    POWER_SRC_CHARGING,   // VUSB_DET HIGH, CHRG_DET LOW
    POWER_SRC_FULL,       // VUSB_DET HIGH, CHRG_DET HIGH
} power_src_t;

typedef struct {
    power_src_t src;
    int vbat_mv;          // last reading; raised by the charge current while charging
    int percent;          // 0-100, only meaningful when vbat_valid
    bool vbat_valid;      // false while charging or before the first good reading
} battery_status_t;

// The R3/R9 divider with C5 has a ~24 ms time constant; VBAT is not read
// until this long after boot (vbat_mv stays 0 until the first reading).
#define BATTERY_SETTLE_MS 150

esp_err_t battery_init(void);
bool battery_usb_present(void);
void battery_update(battery_status_t *out);
// VBAT_SENSE at the ADC pin (VBAT / 2 nominally), averaged; -1 on error
int battery_read_pin_mv(int samples);
int battery_percent_from_mv(int mv);
