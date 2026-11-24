// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Darren Chi
// The load classifier's tuning (load_classify.c): whether a top is loaded,
// which launcher BX Tacho is on, and re-learning a launcher from its
// launches. Shared with the host tests (firmware/test/host), so they test
// these very numbers.
#pragma once

// Hysteresis and debounce (HANDOVER.md 6.2)
#define LOADED_PCT      90
#define RELEASED_PCT    40
#define DEBOUNCE_COUNT  4
// On a switch of launcher, a reading above this on the new one's scale starts loaded
#define SWITCH_LOADED_PCT 50

// A switch to the white launcher needs readings above its loaded threshold for
// this long without a break: a bey pushed in too far on a black launcher lifts
// the pin past it for a moment before it drops back
#define LAUNCHER_WHITE_HOLD_US 3000000

// While empty, slowly track the baseline so drift does not cause false loads
// (load_empty_mv(): the saved calibration stays as it is)
#define BASELINE_TRACK_PCT  20
#define BASELINE_DIV        32  // EMA weight 1/32

// Re-learning a launcher from its launches (load_launch_ended()). On some
// launchers the pin reads higher when empty than the calibration allows: after
// a launch the reading never gets below the released threshold, the top stays
// "loaded" and the next one is never armed. A launch that ended with a result
// means the top has left, whatever the sensor says: if the reading, once
// settled, is clearly below the loaded plateau seen before the pull, that is
// this launcher's empty level. Kept in RAM only (load_relearned()).
//
// The loaded plateau: an EMA of the readings while loaded, outside launches
#define PLATEAU_DIV              8
// Settled: readings within RELEARN_SETTLE_TOL_MV of each other for RELEARN_SETTLE_US;
// not settled within RELEARN_TIMEOUT_US of the end of the launch: nothing learned
#define RELEARN_SETTLE_TOL_MV    10
#define RELEARN_SETTLE_US        300000
#define RELEARN_TIMEOUT_US       3000000
// Clearly below: by at least this share of the saved calibration's span (and
// LOAD_MIN_CONTRAST_MV)
#define RELEARN_MIN_DROP_PCT     30
// Thresholds on the re-learned scale (settled empty level 0 %, plateau 100 %).
// Its ends are raw readings with no margin around them (a saved calibration
// has them built in): the plateau is one top's reading, and the next top may
// sit a little lower. So the thresholds split the drop in thirds, as the web
// app's calibration guide does with real readings.
#define RELEARN_LOADED_PCT       67
#define RELEARN_RELEASED_PCT     33
// Back to the saved calibration: this many readings in a row below its
// released threshold (an empty reading the saved calibration explains, so
// another launcher, or one reading as calibrated)
#define RELEARN_REVERT_COUNT     DEBOUNCE_COUNT
// Event log: EV_LOAD_RELEARN d
#define RELEARN_EV_LEARNED       1
#define RELEARN_EV_REVERTED      0

// Back to the state at boot: black launcher, not loaded, nothing re-learned
// (the host tests, between cases; the firmware starts in it)
void load_classifier_restart(void);
