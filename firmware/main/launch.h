// Launch state machine: EMPTY -> LOADED -> PULLING -> RESULT
// (HANDOVER.md 6.3). Turns rising-edge timestamps into RPM figures.
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    LAUNCH_EMPTY,     // nothing attached (or load sensor not calibrated: armed)
    LAUNCH_LOADED,    // top attached, RPM channel armed
    LAUNCH_PULLING,   // edges arriving, measuring
    LAUNCH_RESULT,    // launch finished, showing the result
} launch_state_t;

typedef enum {
    LAUNCH_END_RELEASE,   // load sensor saw the center part drop
    LAUNCH_END_ABRUPT,    // no release signal, but the edges stopped suddenly
    LAUNCH_END_SLOWDOWN,  // periods lengthened first: the user stopped pulling
} launch_end_t;

typedef struct {
    int peak_rpm;         // median of the three fastest revolutions
    int max_rpm;          // fastest single revolution
    int revolutions;      // complete revolutions counted
    int duration_ms;      // first edge to end of the measurement window
    launch_end_t end;
    int rejected_max_rpm;   // edges dropped: faster than CONFIG_BXT_MAX_RPM
    int rejected_relative;  // edges dropped: too soon for the recent interval
} launch_result_t;

// Continuous RPM meter for bench tests (e.g. a motor spinning the disc).
// Uses the same edge filter as launches; figures cover the last second.
typedef struct {
    int avg_rpm;            // average over the window (0 = stopped)
    int inst_rpm;           // last full revolution
    int min_rpm;            // slowest revolution in the window
    int max_rpm;            // fastest revolution in the window
    int revs;               // revolutions in the window
    uint32_t edges;         // edges accepted since the meter started
    int rejected_max_rpm;   // edges dropped since the meter started
    int rejected_relative;
} meter_stats_t;

void launch_init(void);

// The caller powers the RPM channel; start resets the filter and counters.
void launch_meter_start(void);
void launch_meter_update(meter_stats_t *out);

// Run one step. `loaded` is the debounced load-sensor state, `armed_always`
// keeps the RPM channel armed when there is no load calibration.
// Returns true when something user-visible changed (activity).
bool launch_update(bool loaded, bool armed_always);

launch_state_t launch_get_state(void);
int launch_live_rpm(void);
bool launch_get_result(launch_result_t *out);   // false until the first launch
const char *launch_state_name(launch_state_t s);
