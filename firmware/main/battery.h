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
    int vbat_mv;          // last reading; raised by the charger while USB is present
    int percent;          // 0-100, only meaningful on battery
    bool vbat_valid;      // false while charging or before the first good reading
} battery_status_t;

esp_err_t battery_init(void);
bool battery_usb_present(void);
void battery_update(battery_status_t *out);
int battery_percent_from_mv(int mv);
