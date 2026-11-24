// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Darren Chi
// Fuzz target for the load classifier (firmware/main/load_classify.c), in
// libFuzzer's form (LLVMFuzzerTestOneInput). Where libFuzzer isn't available
// (MSYS2 CLANG64 on Windows), fuzz_main.c runs it on seeded random inputs.
//
// An input is a calibration, two virtual launchers and a list of operations
// (readings without and with a top, launches, stray readings, ADC errors,
// another launcher, a new calibration, `cfg autowhite`). Besides what the
// sanitizers catch (overflow, division by zero, out of bounds), it checks:
//   - a re-learned scale is never narrower than LOAD_MIN_CONTRAST_MV;
//   - a launcher that can be told loaded from empty (on its saved calibration
//     or by re-learning it) never stays stuck: after a launch, the next top
//     loaded arms the next launch.
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "fake.h"
#include "load.h"
#include "load_classify.h"
#include "sim.h"

// ---- the input

#define CAL_MAX_MV          1500   // levels as the ADC can read them (0 .. 3.3 V, ambient-cancelled)
#define SPAN_MAX_MV         600    // |loaded - empty| of a calibration
#define NOISE_MAX_MV        20
#define DURATION_UNIT_US    20000  // an op's duration byte counts in these
#define READING_MIN_MV      (-500)
#define READING_RANGE_MV    3000   // a stray reading: READING_MIN_MV .. + this
#define OPS_MAX             64
// A launch starts with a top fitted for at least this long, and the next top
// can't be fitted sooner than this after it (fetching the top from the
// stadium): a person can't be faster
#define LAUNCH_TOP_US       500000
#define RETRIEVE_US         1000000
// Moving BX Tacho to another launcher: off any launcher for this long, where
// nothing reflects the LED's light (about 0 mV)
#define SWAP_OFF_US         2000000
#define OFF_LAUNCHER_MV     0

enum {
    OP_IDLE,       // readings without a top
    OP_LOAD,       // readings with a top
    OP_LAUNCH,     // a launch (if one is armed)
    OP_READING,    // one stray reading
    OP_ADC_ERROR,  // one failed reading (INT_MIN)
    OP_SWAP,       // the other physical launcher
    OP_NEW_CAL,    // cal load: a new black calibration (in diag mode, then back)
    OP_AUTOWHITE,  // cfg autowhite on/off
    OP_CHECK,      // the property: a cycle after a launch must be armed (when it can be)
    OP_COUNT,
};

// ---- the property's cycle (as the unit tests)
#define CHECK_IDLE_US       1000000
#define CHECK_LOAD_US       1000000
// Margins so the property only claims what the design promises
#define MARGIN_MV           5
#define PCT_SCALE           100

typedef struct {
    const uint8_t *p;
    size_t n;
} reader_t;

static unsigned byte(reader_t *r)
{
    if (r->n == 0) {
        return 0;
    }
    r->n--;
    return *r->p++;
}

static int u16(reader_t *r)
{
    unsigned hi = byte(r);
    return (int)((hi << 8) | byte(r));
}

static int level(reader_t *r)
{
    return u16(r) % (CAL_MAX_MV + 1);
}

static int span(reader_t *r)
{
    return u16(r) % (2 * SPAN_MAX_MV + 1) - SPAN_MAX_MV;
}

static void fail(const char *what)
{
    fprintf(stderr, "property failed: %s\n", what);
    abort();
}

// ---- the state the property needs

static int s_cal_empty, s_cal_loaded;   // black's saved calibration
static bool s_auto_white;
static vlauncher_t s_launchers[2];
static int s_in_use;
static bool s_launched;                 // a launch since the last check

// How often the "never stuck" property was checked, and on re-learned launchers
// (fuzz_main.c reports them: a property never checked proves nothing)
unsigned long fuzz_checks, fuzz_checks_relearned;

static int min_drop(int cal_span)
{
    int d = abs(cal_span) * RELEARN_MIN_DROP_PCT / PCT_SCALE;
    return d < LOAD_MIN_CONTRAST_MV ? LOAD_MIN_CONTRAST_MV : d;
}

// Whether the design promises that this launcher keeps working: black only
// (autowhite off, so it can't be taken for white), a usable calibration that
// rises with the load, readings steady enough to settle, a top reading as
// loaded on the saved calibration, and either an empty reading it calls
// released, or a drop when the top leaves that's enough to re-learn from.
//
// The empty level in use isn't the saved one: baseline tracking moves it to
// readings within BASELINE_TRACK_PCT of the span (the launcher's own empty
// reading, or 0 mV off the launcher), which moves the thresholds and
// re-learning's minimum drop. The promise holds for the worst case of that.
static bool promised(const vlauncher_t *l)
{
    int cal_span = s_cal_loaded - s_cal_empty;
    if (s_auto_white || cal_span < LOAD_MIN_CONTRAST_MV || 2 * l->noise_mv >= RELEARN_SETTLE_TOL_MV) {
        return false;
    }
    int track = cal_span * BASELINE_TRACK_PCT / PCT_SCALE;
    int empty_hi = s_cal_empty + track;   // tracked up: the highest loaded threshold
    int empty_lo = s_cal_empty - track;   // tracked down: the widest span
    int loaded_thr = empty_hi + (s_cal_loaded - empty_hi) * LOADED_PCT / PCT_SCALE;
    int released_thr = empty_lo + (s_cal_loaded - empty_lo) * RELEASED_PCT / PCT_SCALE;
    if (l->loaded_mv - l->noise_mv < loaded_thr + MARGIN_MV) {
        return false;   // a top may not read as loaded
    }
    if (l->empty_mv + l->noise_mv < released_thr - MARGIN_MV) {
        return true;    // as calibrated
    }
    return l->loaded_mv - l->empty_mv - 2 * l->noise_mv >= min_drop(s_cal_loaded - empty_lo) + MARGIN_MV;
}

static void check_invariants(void)
{
    for (int l = 0; l < LAUNCHER_COUNT; l++) {
        int e, ld;
        if (load_relearned(l, &e, &ld) && abs(ld - e) < LOAD_MIN_CONTRAST_MV) {
            fail("re-learned scale narrower than LOAD_MIN_CONTRAST_MV");
        }
    }
    (void)load_last_percent();
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    reader_t r = {data, size};

    s_cal_empty = level(&r);
    s_cal_loaded = s_cal_empty + span(&r);
    int white_empty = level(&r);
    int white_loaded = white_empty + span(&r);
    for (int i = 0; i < 2; i++) {
        s_launchers[i].empty_mv = level(&r);
        s_launchers[i].loaded_mv = level(&r);
        s_launchers[i].noise_mv = (int)(byte(&r) % (NOISE_MAX_MV + 1));
    }
    s_in_use = 0;
    s_launched = false;
    uint32_t seed = (uint32_t)u16(&r) + 1;
    sim_reset(s_cal_empty, s_cal_loaded, white_empty, white_loaded, &s_launchers[0], seed);
    s_auto_white = byte(&r) & 1;
    fake_set_auto_white(s_auto_white);

    // FUZZ_TRACE set: each operation and the classifier's state after it (replaying a crash)
    bool trace = getenv("FUZZ_TRACE") != NULL;
    if (trace) {
        printf("cal black %d/%d white %d/%d, launchers %d/%d (noise %d) and %d/%d (noise %d), autowhite %d\n",
               s_cal_empty, s_cal_loaded, white_empty, white_loaded, s_launchers[0].empty_mv,
               s_launchers[0].loaded_mv, s_launchers[0].noise_mv, s_launchers[1].empty_mv, s_launchers[1].loaded_mv,
               s_launchers[1].noise_mv, s_auto_white);
    }
    for (int op = 0; op < OPS_MAX && r.n > 0; op++) {
        unsigned code = byte(&r) % OP_COUNT;
        int64_t us = (int64_t)(byte(&r) + 1) * DURATION_UNIT_US;
        if (trace) {
            static const char *const NAMES[OP_COUNT] = {"idle", "load", "launch", "reading", "adc error",
                                                        "swap", "new cal", "autowhite", "check"};
            int e = 0, ld = 0;
            bool rl = load_relearned(LAUNCHER_BLACK, &e, &ld);
            printf("%-9s %6lld ms | launcher %d in use, %s, %s, last %d %%, %s, relearned %s %d/%d, autowhite %d, "
                   "cal %d/%d, launches %d\n",
                   NAMES[code], (long long)(us / 1000), s_in_use,
                   load_launcher() == LAUNCHER_WHITE ? "white" : "black", sim_loaded() ? "loaded" : "empty",
                   load_last_percent(), sim_armed() ? "armed" : "not armed", rl ? "yes" : "no", e, ld,
                   s_auto_white, s_cal_empty, s_cal_loaded, sim_launches());
        }
        switch (code) {
        case OP_IDLE:
            sim_idle(us);
            break;
        case OP_LOAD:
            sim_load(us);
            break;
        case OP_LAUNCH:
            sim_load(LAUNCH_TOP_US);
            if (sim_launch()) {
                s_launched = true;
                sim_idle(RETRIEVE_US);
            }
            break;
        case OP_READING:
            sim_sample(READING_MIN_MV + u16(&r) % READING_RANGE_MV);
            break;
        case OP_ADC_ERROR:
            sim_sample(INT_MIN);
            break;
        case OP_SWAP: {
            static const vlauncher_t OFF = {OFF_LAUNCHER_MV, OFF_LAUNCHER_MV, 0};
            sim_use(&OFF);
            sim_idle(SWAP_OFF_US);
            s_in_use ^= 1;
            sim_use(&s_launchers[s_in_use]);
            s_launched = false;   // the promise is about launches on this launcher
            break;
        }
        case OP_NEW_CAL:
            s_cal_empty = level(&r);
            s_cal_loaded = s_cal_empty + span(&r);
            fake_set_cal(LAUNCHER_BLACK, s_cal_empty, s_cal_loaded, true);
            sim_mode_change();    // cal load runs in diag mode: back from it
            s_launched = false;   // the promise is about launches on this calibration
            break;
        case OP_AUTOWHITE:
            s_auto_white = !s_auto_white;
            fake_set_auto_white(s_auto_white);
            break;
        case OP_CHECK:
            // After a launch, and only on the launcher the design makes a promise about
            if (s_launched && promised(&s_launchers[s_in_use])) {
                sim_idle(CHECK_IDLE_US);
                sim_load(CHECK_LOAD_US);
                if (!sim_armed()) {
                    char msg[160];
                    snprintf(msg, sizeof(msg),
                             "stuck: launcher %d/%d mV (noise %d) on calibration %d/%d mV, not armed after a launch",
                             s_launchers[s_in_use].empty_mv, s_launchers[s_in_use].loaded_mv,
                             s_launchers[s_in_use].noise_mv, s_cal_empty, s_cal_loaded);
                    fail(msg);
                }
                fuzz_checks++;
                int e, ld;
                fuzz_checks_relearned += load_relearned(LAUNCHER_BLACK, &e, &ld) ? 1 : 0;
                s_launched = sim_launch();
                sim_idle(RETRIEVE_US);
            }
            break;
        }
        check_invariants();
    }
    return 0;
}
