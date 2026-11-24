// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Darren Chi
// Host tests: what the classifier (load_classify.c) calls outside itself,
// faked and under the test's control: the saved calibration, automatic white
// detection, the clock and the event log.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "eventlog.h"
#include "settings.h"

// The saved calibration of a launcher (settings_cal())
void fake_set_cal(launcher_t launcher, int empty_mv, int loaded_mv, bool valid);
// `cfg autowhite` (settings_auto_white(); default on)
void fake_set_auto_white(bool on);

// The clock (esp_timer_get_time())
void fake_clock_set(int64_t us);
void fake_clock_advance(int64_t us);
int64_t fake_clock_now(void);

// Events logged (eventlog_add_ext()): how many of a type, with this `d`
// (FAKE_ANY_D: any)
#define FAKE_ANY_D INT32_MIN
int fake_events(ev_type_t type, int d);
// The last one of a type, or false if there is none
bool fake_last_event(ev_type_t type, int *a, int *b, int *c, int *d);
void fake_events_clear(void);

// Everything back to the start: no calibration, autowhite on, clock 0, no events
void fake_reset(void);
