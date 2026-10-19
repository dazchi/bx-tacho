// Shared application state between the sensor task, the UI task and the console.
#pragma once

#include <stdbool.h>
#include "battery.h"
#include "launch.h"

typedef enum {
    APP_MODE_RUN,     // normal launch measurement
    APP_MODE_DIAG,    // console drives the sensors directly
    APP_MODE_METER,   // continuous RPM on the display, for bench tests
} app_mode_t;

typedef struct {
    launch_state_t state;
    int live_rpm;
    bool have_result;
    launch_result_t result;

    bool loaded;
    bool load_cal_valid;
    int load_diff_mv;         // last ambient-cancelled LOAD_SENSE reading
    int load_pct;             // position between empty (0) and loaded (100)

    battery_status_t batt;
    app_mode_t mode;
    meter_stats_t meter;      // valid in APP_MODE_METER
    uint32_t idle_ms;         // time since the last user-visible activity
} app_snapshot_t;

void app_get_snapshot(app_snapshot_t *out);

// DIAG and METER pause the launch state machine. DIAG lets the console drive
// the sensors directly; METER keeps the RPM channel on and shows live RPM.
void app_set_mode(app_mode_t mode);
app_mode_t app_get_mode(void);
const char *app_mode_name(app_mode_t mode);

void app_note_activity(void);
void app_enter_deep_sleep(void);
