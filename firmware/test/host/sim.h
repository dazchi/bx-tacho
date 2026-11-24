// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Darren Chi
// Host tests: a virtual launcher feeding LOAD_SENSE readings to the classifier
// (load_classify.c) the way the sensor task does (main.c), and the bit of the
// launch state machine (launch.c) that decides whether a launch is armed.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "settings.h"

// How often the sensor task reads the load sensor (main.c): polled while
// empty, a short flash while loaded or during a launch
#define SIM_POLL_US          50000   // LOAD_POLL_MS
#define SIM_FAST_US          10000   // LOAD_FLASH_IDLE_US

// A launch as the sim plays it: the top stays on the pin for SIM_RELEASE_US
// of the pull, then the disc spins down for SIM_SPIN_US (no load readings
// change the launcher meanwhile), then the launch ends with a result
#define SIM_RELEASE_US       150000
#define SIM_SPIN_US          2000000

// What a physical launcher reads (ambient-cancelled mV), with a top on it and
// without, and the noise on each reading (uniform, +/- noise_mv)
typedef struct {
    int empty_mv;
    int loaded_mv;
    int noise_mv;
} vlauncher_t;

// A fresh classifier (load_classifier_restart()) and clock, no events, both
// launchers' saved calibrations (valid), automatic white detection on, the
// noise generator seeded, and `launcher` on BX Tacho without a top
void sim_reset(int black_empty, int black_loaded, int white_empty, int white_loaded, const vlauncher_t *launcher,
               uint32_t seed);
// Another physical launcher from now on (the top as it was)
void sim_use(const vlauncher_t *launcher);

// Readings for `us`: without a top (sim_idle) or with one (sim_load)
void sim_idle(int64_t us);
void sim_load(int64_t us);
// One reading of `mv` (no noise) and the next sample time
void sim_sample(int mv);

// A launch: only if one is armed (a fresh load since the last launch, as
// launch.c needs); false if not, and then nothing happens. Afterwards there
// is no top on the launcher.
bool sim_launch(void);

// Back from another mode (diag, where `cal load` runs): main.c resets the
// classifier's loaded state and the launch state machine (launch_init())
void sim_mode_change(void);

bool sim_armed(void);    // a load seen since the last launch: the next launch is measured
bool sim_loaded(void);   // what the classifier said last
int sim_launches(void);  // launches measured since sim_reset()
