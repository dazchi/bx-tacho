// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Darren Chi
// Unit tests of the load classifier (firmware/main/load_classify.c): loads and
// releases, launches re-armed, the launcher in use, and re-learning a launcher
// that reads higher than calibrated. Virtual launchers (sim.h) feed it; the
// thresholds are the firmware's own (load_classify.h).
#include <limits.h>
#include <stdio.h>
#include "fake.h"
#include "fixtures/launcher_cals.h"
#include "load.h"
#include "load_classify.h"
#include "sim.h"
#include "unity.h"

// ---- scenarios

// The default calibration (settings.c, settings_reset())
#define DEF_BLACK_EMPTY      100
#define DEF_BLACK_LOADED     200
#define DEF_WHITE_EMPTY      490
#define DEF_WHITE_LOADED     690

#define NOISE_MV             3         // per reading, +/-: well inside RELEARN_SETTLE_TOL_MV
#define SEED                 1

// A load-and-launch cycle: off the launcher, then a top on it, then the launch
#define IDLE_US              1000000
#define LOAD_US              1000000
#define LAUNCHES             10        // in a row, each must be armed
#define FIXTURE_LAUNCHES     5

// A launcher that reads higher than calibrated (the reported problem): its empty
// pin at STUCK_EMPTY_PCT of the calibrated span, a top STUCK_LOADED_PCT
#define STUCK_EMPTY_PCT      55        // above RELEASED_PCT: never released on the saved calibration
#define STUCK_LOADED_PCT     110
// One whose reading barely changes when the top leaves: nothing to learn from
#define FLAT_EMPTY_PCT       95
#define FLAT_LOADED_PCT      105

// Switching to white needs LAUNCHER_WHITE_HOLD_US above its threshold; a bit more
#define WHITE_SWITCH_MARGIN_US 200000

// Readings that never settle after a launch: alternately this far apart
#define UNSETTLED_SWING_MV   (2 * RELEARN_SETTLE_TOL_MV)

#define PCT_SCALE            100       // percent

static int at_pct(int empty, int loaded, int pct)
{
    return empty + (loaded - empty) * pct / PCT_SCALE;
}

static bool cycle(void)
{
    sim_idle(IDLE_US);
    sim_load(LOAD_US);
    return sim_launch();
}

// `n` cycles; how many launches were armed
static int cycles(int n)
{
    int armed = 0;
    for (int i = 0; i < n; i++) {
        armed += cycle() ? 1 : 0;
    }
    return armed;
}

static int relearned_events(void)
{
    return fake_events(EV_LOAD_RELEARN, RELEARN_EV_LEARNED);
}

static int reverted_events(void)
{
    return fake_events(EV_LOAD_RELEARN, RELEARN_EV_REVERTED);
}

static vlauncher_t s_black, s_white, s_stuck, s_flat;

void setUp(void)
{
    s_black = (vlauncher_t){DEF_BLACK_EMPTY, DEF_BLACK_LOADED, NOISE_MV};
    s_white = (vlauncher_t){DEF_WHITE_EMPTY, DEF_WHITE_LOADED, NOISE_MV};
    s_stuck = (vlauncher_t){at_pct(DEF_BLACK_EMPTY, DEF_BLACK_LOADED, STUCK_EMPTY_PCT),
                            at_pct(DEF_BLACK_EMPTY, DEF_BLACK_LOADED, STUCK_LOADED_PCT), NOISE_MV};
    s_flat = (vlauncher_t){at_pct(DEF_BLACK_EMPTY, DEF_BLACK_LOADED, FLAT_EMPTY_PCT),
                           at_pct(DEF_BLACK_EMPTY, DEF_BLACK_LOADED, FLAT_LOADED_PCT), NOISE_MV};
}

void tearDown(void)
{
}

static void reset_with(const vlauncher_t *l)
{
    sim_reset(DEF_BLACK_EMPTY, DEF_BLACK_LOADED, DEF_WHITE_EMPTY, DEF_WHITE_LOADED, l, SEED);
}

// ---- debounce

static void test_needs_debounce_count_readings_to_load(void)
{
    reset_with(&s_black);
    sim_idle(IDLE_US);
    for (int i = 0; i < DEBOUNCE_COUNT - 1; i++) {
        sim_sample(DEF_BLACK_LOADED);
    }
    TEST_ASSERT_FALSE(sim_loaded());
    sim_sample(DEF_BLACK_LOADED);
    TEST_ASSERT_TRUE(sim_loaded());
}

static void test_single_spikes_never_load(void)
{
    reset_with(&s_black);
    for (int i = 0; i < LAUNCHES; i++) {
        sim_sample(DEF_BLACK_LOADED);
        sim_idle(SIM_POLL_US * DEBOUNCE_COUNT);
        TEST_ASSERT_FALSE(sim_loaded());
    }
    TEST_ASSERT_FALSE(sim_armed());
}

static void test_adc_error_reads_as_not_loaded(void)
{
    reset_with(&s_black);
    sim_load(LOAD_US);
    TEST_ASSERT_TRUE(sim_loaded());
    sim_sample(INT_MIN);
    TEST_ASSERT_FALSE(sim_loaded());
}

// ---- launchers as calibrated

static void test_black_launches_are_rearmed(void)
{
    reset_with(&s_black);
    TEST_ASSERT_EQUAL_INT(LAUNCHES, cycles(LAUNCHES));
    TEST_ASSERT_EQUAL_INT(LAUNCHER_BLACK, load_launcher());
    TEST_ASSERT_EQUAL_INT(0, relearned_events());
}

static void test_white_launcher_is_detected_and_rearmed(void)
{
    reset_with(&s_white);
    sim_load(LAUNCHER_WHITE_HOLD_US + WHITE_SWITCH_MARGIN_US);
    TEST_ASSERT_EQUAL_INT(LAUNCHER_WHITE, load_launcher());
    TEST_ASSERT_TRUE(sim_launch());
    TEST_ASSERT_EQUAL_INT(LAUNCHES, cycles(LAUNCHES));
    TEST_ASSERT_EQUAL_INT(LAUNCHER_WHITE, load_launcher());
    TEST_ASSERT_EQUAL_INT(0, relearned_events());
}

static void test_back_to_black_after_white(void)
{
    reset_with(&s_white);
    sim_load(LAUNCHER_WHITE_HOLD_US + WHITE_SWITCH_MARGIN_US);
    TEST_ASSERT_EQUAL_INT(LAUNCHER_WHITE, load_launcher());
    sim_use(&s_black);
    sim_idle(IDLE_US);
    TEST_ASSERT_EQUAL_INT(LAUNCHER_BLACK, load_launcher());
    TEST_ASSERT_EQUAL_INT(LAUNCHES, cycles(LAUNCHES));
}

static void test_autowhite_off_stays_black(void)
{
    reset_with(&s_white);
    fake_set_auto_white(false);
    sim_load(LAUNCHER_WHITE_HOLD_US + WHITE_SWITCH_MARGIN_US);
    TEST_ASSERT_EQUAL_INT(LAUNCHER_BLACK, load_launcher());
}

// ---- a launcher that reads higher than calibrated (re-learned from its launches)

static void test_stuck_launcher_still_loaded_right_after_launch(void)
{
    // The reported problem, as the saved calibration alone sees it
    reset_with(&s_stuck);
    TEST_ASSERT_TRUE(cycle());
    sim_sample(s_stuck.empty_mv);
    TEST_ASSERT_TRUE(sim_loaded());
    TEST_ASSERT_EQUAL_INT(0, relearned_events());
}

static void test_stuck_launcher_is_relearned_and_rearmed(void)
{
    reset_with(&s_stuck);
    TEST_ASSERT_EQUAL_INT(LAUNCHES, cycles(LAUNCHES));
    TEST_ASSERT_EQUAL_INT(1, relearned_events());
    int empty, loaded;
    TEST_ASSERT_TRUE(load_relearned(LAUNCHER_BLACK, &empty, &loaded));
    TEST_ASSERT_INT_WITHIN(NOISE_MV + RELEARN_SETTLE_TOL_MV, s_stuck.empty_mv, empty);
    TEST_ASSERT_INT_WITHIN(NOISE_MV + RELEARN_SETTLE_TOL_MV, s_stuck.loaded_mv, loaded);
}

static void test_relearn_event_has_the_levels(void)
{
    reset_with(&s_stuck);
    cycles(2);
    int a, b, c, d;
    TEST_ASSERT_TRUE(fake_last_event(EV_LOAD_RELEARN, &a, &b, &c, &d));
    TEST_ASSERT_EQUAL_INT(RELEARN_EV_LEARNED, d);
    TEST_ASSERT_EQUAL_INT(LAUNCHER_BLACK, c);
    TEST_ASSERT_INT_WITHIN(NOISE_MV + RELEARN_SETTLE_TOL_MV, s_stuck.empty_mv, a);
    TEST_ASSERT_INT_WITHIN(NOISE_MV + RELEARN_SETTLE_TOL_MV, s_stuck.loaded_mv, b);
}

static void test_relearn_reverts_on_a_launcher_as_calibrated(void)
{
    reset_with(&s_stuck);
    cycles(2);
    TEST_ASSERT_EQUAL_INT(1, relearned_events());
    sim_use(&s_black);
    sim_idle(IDLE_US);
    TEST_ASSERT_EQUAL_INT(1, reverted_events());
    int empty, loaded;
    TEST_ASSERT_FALSE(load_relearned(LAUNCHER_BLACK, &empty, &loaded));
    TEST_ASSERT_EQUAL_INT(LAUNCHES, cycles(LAUNCHES));
}

static void test_new_calibration_drops_the_relearned_levels(void)
{
    reset_with(&s_stuck);
    cycles(2);
    int empty, loaded;
    TEST_ASSERT_TRUE(load_relearned(LAUNCHER_BLACK, &empty, &loaded));
    fake_set_cal(LAUNCHER_BLACK, s_stuck.empty_mv, s_stuck.loaded_mv, true);   // cal load
    TEST_ASSERT_FALSE(load_relearned(LAUNCHER_BLACK, &empty, &loaded));
}

static void test_normal_launcher_never_relearns(void)
{
    reset_with(&s_black);
    cycles(LAUNCHES);
    int empty, loaded;
    TEST_ASSERT_FALSE(load_relearned(LAUNCHER_BLACK, &empty, &loaded));
    TEST_ASSERT_EQUAL_INT(0, fake_events(EV_LOAD_RELEARN, FAKE_ANY_D));
}

static void test_unsettled_reading_learns_nothing(void)
{
    reset_with(&s_stuck);
    TEST_ASSERT_TRUE(cycle());
    // Readings swinging more than the tolerance until the timeout has passed
    int64_t end = fake_clock_now() + RELEARN_TIMEOUT_US + IDLE_US;
    for (int i = 0; fake_clock_now() < end; i++) {
        sim_sample(s_stuck.empty_mv + (i % 2 ? UNSETTLED_SWING_MV : 0));
    }
    TEST_ASSERT_EQUAL_INT(0, relearned_events());
    TEST_ASSERT_TRUE(sim_loaded());
}

static void test_flat_launcher_stays_stuck(void)
{
    // A known limit: the reading barely drops when the top leaves, so there's
    // nothing to learn from and only the first launch is armed (needs cal load)
    reset_with(&s_flat);
    TEST_ASSERT_TRUE(cycle());
    TEST_ASSERT_FALSE(cycle());
    TEST_ASSERT_EQUAL_INT(0, relearned_events());
}

static void test_quick_reload_after_launch_learns_nothing(void)
{
    // A known limit: a top fitted again before the empty reading has settled
    // (RELEARN_SETTLE_US) ends the settling on the loaded level, with no drop
    // to learn from. On a stuck launcher no fresh load can follow, so no launch
    // either: stuck until a restart or another launcher. (Fetching the top from
    // the stadium takes longer, so this needs an unusually quick reload.)
    reset_with(&s_stuck);
    TEST_ASSERT_TRUE(cycle());
    sim_load(LOAD_US);
    TEST_ASSERT_FALSE(cycle());
    TEST_ASSERT_EQUAL_INT(0, relearned_events());
}

static void test_stuck_white_launcher_is_relearned(void)
{
    vlauncher_t stuck_white = {at_pct(DEF_WHITE_EMPTY, DEF_WHITE_LOADED, STUCK_EMPTY_PCT),
                               at_pct(DEF_WHITE_EMPTY, DEF_WHITE_LOADED, STUCK_LOADED_PCT), NOISE_MV};
    reset_with(&stuck_white);
    sim_load(LAUNCHER_WHITE_HOLD_US + WHITE_SWITCH_MARGIN_US);
    TEST_ASSERT_EQUAL_INT(LAUNCHER_WHITE, load_launcher());
    TEST_ASSERT_TRUE(sim_launch());
    TEST_ASSERT_EQUAL_INT(LAUNCHES, cycles(LAUNCHES));
    int empty, loaded;
    TEST_ASSERT_TRUE(load_relearned(LAUNCHER_WHITE, &empty, &loaded));
}

// ---- the calibrations of the boards made so far (fixtures/launcher_cals.h)

static void test_fixture_launchers_as_calibrated(void)
{
    for (size_t i = 0; i < FIXTURE_CAL_COUNT; i++) {
        const fixture_cal_t *f = &FIXTURE_CALS[i];
        vlauncher_t black = {f->black_empty, f->black_loaded, NOISE_MV};
        vlauncher_t white = {f->white_empty, f->white_loaded, NOISE_MV};
        char msg[96];

        sim_reset(f->black_empty, f->black_loaded, f->white_empty, f->white_loaded, &black, SEED);
        snprintf(msg, sizeof(msg), "black, fixture %u", (unsigned)i);
        TEST_ASSERT_EQUAL_INT_MESSAGE(FIXTURE_LAUNCHES, cycles(FIXTURE_LAUNCHES), msg);
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, relearned_events(), msg);

        sim_reset(f->black_empty, f->black_loaded, f->white_empty, f->white_loaded, &white, SEED);
        sim_load(LAUNCHER_WHITE_HOLD_US + WHITE_SWITCH_MARGIN_US);
        snprintf(msg, sizeof(msg), "white, fixture %u", (unsigned)i);
        TEST_ASSERT_EQUAL_INT_MESSAGE(LAUNCHER_WHITE, load_launcher(), msg);
        TEST_ASSERT_TRUE_MESSAGE(sim_launch(), msg);
        TEST_ASSERT_EQUAL_INT_MESSAGE(FIXTURE_LAUNCHES, cycles(FIXTURE_LAUNCHES), msg);
    }
}

// The stuck variant of each board's black calibration. Re-learning needs the
// top to leave a drop of RELEARN_MIN_DROP_PCT of the span and at least
// LOAD_MIN_CONTRAST_MV: on a narrow calibration that absolute minimum can be
// more than the stuck launcher's drop. Those are counted and reported, not failed.
static void test_fixture_stuck_launchers_recover(void)
{
    int narrow = 0;
    for (size_t i = 0; i < FIXTURE_CAL_COUNT; i++) {
        const fixture_cal_t *f = &FIXTURE_CALS[i];
        vlauncher_t stuck = {at_pct(f->black_empty, f->black_loaded, STUCK_EMPTY_PCT),
                             at_pct(f->black_empty, f->black_loaded, STUCK_LOADED_PCT), NOISE_MV};
        int span = f->black_loaded - f->black_empty;
        int min_drop = span * RELEARN_MIN_DROP_PCT / PCT_SCALE;
        min_drop = min_drop < LOAD_MIN_CONTRAST_MV ? LOAD_MIN_CONTRAST_MV : min_drop;
        bool learnable = stuck.loaded_mv - stuck.empty_mv - 2 * NOISE_MV >= min_drop;
        char msg[128];
        snprintf(msg, sizeof(msg), "fixture %u (black %d/%d, stuck launcher %d/%d)", (unsigned)i, f->black_empty,
                 f->black_loaded, stuck.empty_mv, stuck.loaded_mv);

        sim_reset(f->black_empty, f->black_loaded, f->white_empty, f->white_loaded, &stuck, SEED);
        int armed = cycles(FIXTURE_LAUNCHES);
        if (learnable) {
            TEST_ASSERT_EQUAL_INT_MESSAGE(FIXTURE_LAUNCHES, armed, msg);
            TEST_ASSERT_EQUAL_INT_MESSAGE(1, relearned_events(), msg);
        } else if (armed < FIXTURE_LAUNCHES) {
            narrow++;
            printf("  stays stuck (drop under %d mV): %s, %d of %d launches armed\n", min_drop, msg, armed,
                   FIXTURE_LAUNCHES);
        }
    }
    printf("  %d of %u board calibrations can't re-learn a launcher %d %% high\n", narrow,
           (unsigned)FIXTURE_CAL_COUNT, STUCK_EMPTY_PCT);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_needs_debounce_count_readings_to_load);
    RUN_TEST(test_single_spikes_never_load);
    RUN_TEST(test_adc_error_reads_as_not_loaded);
    RUN_TEST(test_black_launches_are_rearmed);
    RUN_TEST(test_white_launcher_is_detected_and_rearmed);
    RUN_TEST(test_back_to_black_after_white);
    RUN_TEST(test_autowhite_off_stays_black);
    RUN_TEST(test_stuck_launcher_still_loaded_right_after_launch);
    RUN_TEST(test_stuck_launcher_is_relearned_and_rearmed);
    RUN_TEST(test_relearn_event_has_the_levels);
    RUN_TEST(test_relearn_reverts_on_a_launcher_as_calibrated);
    RUN_TEST(test_new_calibration_drops_the_relearned_levels);
    RUN_TEST(test_normal_launcher_never_relearns);
    RUN_TEST(test_unsettled_reading_learns_nothing);
    RUN_TEST(test_flat_launcher_stays_stuck);
    RUN_TEST(test_quick_reload_after_launch_learns_nothing);
    RUN_TEST(test_stuck_white_launcher_is_relearned);
    RUN_TEST(test_fixture_launchers_as_calibrated);
    RUN_TEST(test_fixture_stuck_launchers_recover);
    return UNITY_END();
}
