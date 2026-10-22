// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Darren Chi
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
// True from the release (seen by the load sensor) until the launch ends
bool launch_released(void);
bool launch_get_result(launch_result_t *out);   // false until the first launch

// Revolution periods of the last launch in time order, from the start of the
// pull, including the spin-down after the release (the Battle Pass curve).
// Returns how many were copied, at most `max`.
#define LAUNCH_CURVE_MAX 32
int launch_get_curve(uint32_t *periods_us, int max);

// Raw record of the last launch, for the console: edge timestamps (us), the
// release seen by the load sensor, and the edges the chatter filter dropped.
#define LAUNCH_TRACE_REJECTS 48
typedef struct {
    int num_edges;            // accepted edges in the launch (all of them, not just those copied)
    bool release_seen;
    uint32_t release_us;      // same clock as the edges
    int num_rejected;         // dropped edges stored below (the first LAUNCH_TRACE_REJECTS)
    int lock_recoveries;      // times the filter was found locked and the dropped edges restored
    uint32_t rejected_us[LAUNCH_TRACE_REJECTS];
    bool rejected_max_rpm[LAUNCH_TRACE_REJECTS];   // true: faster than the max; false: too soon (relative)
} launch_trace_t;

// Copies the first `max` edges of the last launch. Returns false if there is
// no finished launch (none yet, or one in progress).
bool launch_get_trace(uint32_t *edges, int max, launch_trace_t *info);
const char *launch_state_name(launch_state_t s);
