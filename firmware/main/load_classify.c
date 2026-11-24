// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Darren Chi
// The load classifier: from the ambient-cancelled LOAD_SENSE readings
// (load.c measures them), whether a top is loaded and which launcher BX Tacho
// is on. No hardware here: the host tests (firmware/test/host) build this file
// as it is, with settings, the clock and the event log stubbed.
#include "load.h"
#include "load_classify.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include "esp_timer.h"
#include "eventlog.h"
#include "settings.h"

static bool s_loaded;
static int s_streak;
static int s_last_pct;

static launcher_t s_launcher;   // black until a reading says otherwise
static int s_launcher_streak;     // readings in a row pointing to black
static bool s_white_pending;      // readings above the white threshold...
static int64_t s_white_since_us;  // ...without a break since then

// The empty level in use, per launcher (load_empty_mv()), and the calibration
// it was started from: a new one (cal load, cfg reset) starts it again
static int s_empty_mv[LAUNCHER_COUNT];
static launcher_cal_t s_empty_from[LAUNCHER_COUNT];
static bool s_empty_init[LAUNCHER_COUNT];

// Re-learned from launches (load_relearned()), per launcher
typedef struct {
    bool active;
    int empty_mv;      // settled after a launch
    int loaded_mv;     // the plateau before it
    int revert_streak; // readings in a row the saved calibration calls released
} relearn_t;
static relearn_t s_relearn[LAUNCHER_COUNT];

// The loaded plateau of the current top, and the settling after a launch
static int s_plateau_mv;
static bool s_plateau_valid;
static bool s_settle_pending;    // a launch ended with a result: wait for the reading to settle
static int64_t s_settle_end_us;  // give up then
static int64_t s_window_us;      // readings since then are within the tolerance
static int s_window_min;
static int s_window_max;
static int64_t s_window_sum;
static int s_window_n;

void load_classifier_restart(void)
{
    s_loaded = false;
    s_streak = 0;
    s_last_pct = 0;
    s_launcher = LAUNCHER_BLACK;
    s_launcher_streak = 0;
    s_white_pending = false;
    s_white_since_us = 0;
    memset(s_empty_init, 0, sizeof(s_empty_init));
    memset(s_relearn, 0, sizeof(s_relearn));
    s_plateau_valid = false;
    s_settle_pending = false;
    s_window_n = 0;
}

void load_classifier_reset(bool loaded)
{
    s_loaded = loaded;
    s_streak = 0;
}

int load_last_percent(void)
{
    return s_last_pct;
}

launcher_t load_launcher(void)
{
    return s_launcher;
}

// The re-learned empty level of a launcher, started again from its saved
// calibration whenever that changed
static int *empty_level(launcher_t launcher)
{
    int l = launcher == LAUNCHER_WHITE ? LAUNCHER_WHITE : LAUNCHER_BLACK;
    const launcher_cal_t *cal = settings_cal(l);
    const launcher_cal_t *from = &s_empty_from[l];
    if (!s_empty_init[l] || cal->load_empty != from->load_empty || cal->load_loaded != from->load_loaded ||
        cal->load_cal_valid != from->load_cal_valid) {
        s_empty_from[l] = *cal;
        s_empty_mv[l] = cal->load_empty;
        s_empty_init[l] = true;
        s_relearn[l].active = false;
    }
    return &s_empty_mv[l];
}

int load_empty_mv(launcher_t launcher)
{
    return *empty_level(launcher);
}

// Position of a reading between a launcher's empty (0, as re-learned) and
// loaded (100) levels on its saved calibration, or false without a usable one
static bool saved_percent(launcher_t launcher, int diff_mv, int *pct)
{
    const launcher_cal_t *cal = settings_cal(launcher);
    int empty = load_empty_mv(launcher);
    int span = cal->load_loaded - empty;
    if (!cal->load_cal_valid || abs(span) < LOAD_MIN_CONTRAST_MV) {
        return false;
    }
    *pct = (diff_mv - empty) * 100 / span;
    return true;
}

static relearn_t *relearn_of(launcher_t launcher)
{
    empty_level(launcher);   // a changed calibration drops it
    return &s_relearn[launcher == LAUNCHER_WHITE ? LAUNCHER_WHITE : LAUNCHER_BLACK];
}

// The same on the scale in use: the one re-learned from launches, if there is one
static bool cal_percent(launcher_t launcher, int diff_mv, int *pct)
{
    const relearn_t *r = relearn_of(launcher);
    if (!r->active) {
        return saved_percent(launcher, diff_mv, pct);
    }
    *pct = (diff_mv - r->empty_mv) * 100 / (r->loaded_mv - r->empty_mv);
    return true;
}

// The thresholds on that scale
static int loaded_pct(launcher_t launcher)
{
    return relearn_of(launcher)->active ? RELEARN_LOADED_PCT : LOADED_PCT;
}

static int released_pct(launcher_t launcher)
{
    return relearn_of(launcher)->active ? RELEARN_RELEASED_PCT : RELEASED_PCT;
}

bool load_relearned(launcher_t launcher, int *empty_mv, int *loaded_mv)
{
    const relearn_t *r = relearn_of(launcher);
    if (r->active) {
        *empty_mv = r->empty_mv;
        *loaded_mv = r->loaded_mv;
    }
    return r->active;
}

void load_launch_ended(void)
{
    s_settle_pending = true;
    s_settle_end_us = esp_timer_get_time() + RELEARN_TIMEOUT_US;
    s_window_n = 0;
}

// While the top still counts as loaded after a launch that ended with a result:
// once the reading has settled clearly below the plateau, that is the empty
// level, and the top has left
static void settle_after_launch(launcher_t launcher, int diff_mv)
{
    int64_t now = esp_timer_get_time();
    if (!s_loaded || !s_plateau_valid || now > s_settle_end_us) {
        s_settle_pending = false;   // released as usual, nothing to learn from, or never settled
        return;
    }
    if (s_window_n > 0) {
        int lo = diff_mv < s_window_min ? diff_mv : s_window_min;
        int hi = diff_mv > s_window_max ? diff_mv : s_window_max;
        if (hi - lo > RELEARN_SETTLE_TOL_MV) {
            s_window_n = 0;   // still moving: start the window again here
        }
    }
    if (s_window_n == 0) {
        s_window_us = now;
        s_window_min = s_window_max = diff_mv;
        s_window_sum = 0;
    }
    s_window_min = diff_mv < s_window_min ? diff_mv : s_window_min;
    s_window_max = diff_mv > s_window_max ? diff_mv : s_window_max;
    s_window_sum += diff_mv;
    s_window_n++;
    if (now - s_window_us < RELEARN_SETTLE_US) {
        return;
    }
    s_settle_pending = false;
    int settled = (int)(s_window_sum / s_window_n);

    // Clearly below the plateau, in the direction the saved calibration goes
    const launcher_cal_t *cal = settings_cal(launcher);
    int span = cal->load_loaded - load_empty_mv(launcher);
    int dir = span >= 0 ? 1 : -1;
    int drop = (s_plateau_mv - settled) * dir;
    int min_drop = abs(span) * RELEARN_MIN_DROP_PCT / 100;
    if (min_drop < LOAD_MIN_CONTRAST_MV) {
        min_drop = LOAD_MIN_CONTRAST_MV;
    }
    if (!cal->load_cal_valid || drop < min_drop) {
        return;   // no contrast to go by: it stays as it is
    }
    relearn_t *r = relearn_of(launcher);
    r->active = true;
    r->empty_mv = settled;
    r->loaded_mv = s_plateau_mv;
    r->revert_streak = 0;
    s_loaded = false;
    s_streak = 0;
    eventlog_add_ext(EV_LOAD_RELEARN, settled, s_plateau_mv, launcher, RELEARN_EV_LEARNED, 0);
}

// Back to the saved calibration once it explains the readings again
static void check_revert(launcher_t launcher, int diff_mv)
{
    relearn_t *r = relearn_of(launcher);
    int pct;
    if (!r->active || s_loaded || !saved_percent(launcher, diff_mv, &pct)) {
        return;
    }
    if (pct >= RELEASED_PCT) {
        r->revert_streak = 0;
    } else if (++r->revert_streak >= RELEARN_REVERT_COUNT) {
        r->active = false;
        s_streak = 0;
        eventlog_add_ext(EV_LOAD_RELEARN, load_empty_mv(launcher), settings_cal(launcher)->load_loaded, launcher,
                         RELEARN_EV_REVERTED, 0);
    }
}

launcher_t load_track_launcher(int diff_mv, bool may_switch)
{
    if (diff_mv == INT_MIN) {
        return s_launcher;
    }
    // Above the white launcher's loaded level: only a white launcher reads that
    // high (a top on it, or its pin). Below the black one's released level: only
    // a black launcher (or none) reads that low. In between (an empty white
    // launcher, the pin pushed up on a black one) it stays as it is.
    // With automatic white detection off ('cfg autowhite off'), always black.
    int pct;
    bool black_only = !settings_auto_white();
    launcher_t seen = s_launcher;
    if (black_only) {
        seen = LAUNCHER_BLACK;
    } else if (cal_percent(LAUNCHER_WHITE, diff_mv, &pct) && pct > loaded_pct(LAUNCHER_WHITE)) {
        seen = LAUNCHER_WHITE;
    } else if (cal_percent(LAUNCHER_BLACK, diff_mv, &pct) && pct < released_pct(LAUNCHER_BLACK)) {
        seen = LAUNCHER_BLACK;
    }
    bool change = false;
    if (seen == s_launcher || !may_switch) {
        s_launcher_streak = 0;
        s_white_pending = false;
    } else if (black_only) {
        change = true;   // just switched off while on white: back to black at once
    } else if (seen == LAUNCHER_WHITE) {
        int64_t now = esp_timer_get_time();
        if (!s_white_pending) {
            s_white_pending = true;
            s_white_since_us = now;
        } else if (now - s_white_since_us >= LAUNCHER_WHITE_HOLD_US) {
            change = true;
        }
    } else if (++s_launcher_streak >= DEBOUNCE_COUNT) {
        change = true;
    }
    if (change) {
        s_launcher = seen;
        s_launcher_streak = 0;
        s_white_pending = false;
        // Start from where this reading lies on the new launcher's scale, not from
        // the old state, so the switch itself isn't taken for a load or a release
        int pct;
        s_loaded = cal_percent(seen, diff_mv, &pct) && pct > SWITCH_LOADED_PCT;
        s_streak = 0;
        s_plateau_valid = false;
        s_settle_pending = false;
    }
    return s_launcher;
}

bool load_classify(int diff_mv, bool may_switch)
{
    launcher_t launcher = load_track_launcher(diff_mv, may_switch);
    int pct;   // works whichever way the signal moves when the center part rises

    if (diff_mv == INT_MIN || !cal_percent(launcher, diff_mv, &pct)) {
        s_loaded = false;
        s_streak = 0;
        s_plateau_valid = false;
        s_settle_pending = false;
        return false;
    }

    if (!may_switch) {
        s_settle_pending = false;   // a new launch: the last one has nothing more to say
    } else if (s_settle_pending) {
        settle_after_launch(launcher, diff_mv);
    }
    check_revert(launcher, diff_mv);
    cal_percent(launcher, diff_mv, &pct);   // on the scale in use now (it may have changed just above)
    s_last_pct = pct;

    bool was_loaded = s_loaded;
    bool crossing = s_loaded ? (pct < released_pct(launcher)) : (pct > loaded_pct(launcher));
    if (crossing) {
        if (++s_streak >= DEBOUNCE_COUNT) {
            s_loaded = !s_loaded;
            s_streak = 0;
        }
    } else {
        s_streak = 0;
    }

    // The loaded plateau of this top: while it sits loaded, not during a launch
    // nor while settling after one (the top has left then)
    if (!s_loaded) {
        s_plateau_valid = false;
    } else if (may_switch && !s_settle_pending) {
        if (!was_loaded || !s_plateau_valid) {
            s_plateau_mv = diff_mv;
            s_plateau_valid = true;
        } else {
            s_plateau_mv += (diff_mv - s_plateau_mv) / PLATEAU_DIV;
        }
    }

    // Re-learn the empty baseline while idle (load_empty_mv(): RAM only, the
    // saved calibration isn't changed), only near the empty level: readings off
    // the launcher (about 0 mV) would otherwise drag it down, and it couldn't
    // come back up
    if (!s_loaded && pct > -BASELINE_TRACK_PCT && pct < BASELINE_TRACK_PCT && s_launcher_streak == 0 &&
        !s_white_pending) {
        relearn_t *r = relearn_of(launcher);
        int *empty = r->active ? &r->empty_mv : empty_level(launcher);
        *empty += (diff_mv - *empty) / BASELINE_DIV;
    }
    return s_loaded;
}
