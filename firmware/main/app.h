// Shared application state between the sensor task, the UI task and the console.
#pragma once

#include <stdbool.h>
#include "battery.h"
#include "launch.h"

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
    bool diag;                // console has taken over the sensors
    uint32_t idle_ms;         // time since the last user-visible activity
} app_snapshot_t;

void app_get_snapshot(app_snapshot_t *out);

// Diagnostic mode pauses the launch state machine so the console can drive
// the sensors directly.
void app_set_diag(bool on);
bool app_is_diag(void);

void app_note_activity(void);
void app_enter_deep_sleep(void);
