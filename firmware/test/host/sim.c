// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Darren Chi
#include "sim.h"

#include "fake.h"
#include "load.h"
#include "load_classify.h"

// xorshift32: the same noise for the same seed, on every platform
#define SIM_SEED_DEFAULT 0x9E3779B9u
#define XORSHIFT_A       13
#define XORSHIFT_B       17
#define XORSHIFT_C       5

static const vlauncher_t *s_launcher;
static bool s_top;
static bool s_pulling;
static bool s_armed;
static bool s_was_loaded;
static bool s_loaded;
static int s_launches;
static launcher_t s_prev_launcher;
static uint32_t s_rng;

static uint32_t rnd(void)
{
    s_rng ^= s_rng << XORSHIFT_A;
    s_rng ^= s_rng >> XORSHIFT_B;
    s_rng ^= s_rng << XORSHIFT_C;
    return s_rng;
}

static int noise(int amplitude)
{
    if (amplitude <= 0) {
        return 0;
    }
    return (int)(rnd() % (uint32_t)(2 * amplitude + 1)) - amplitude;
}

static int reading(void)
{
    return (s_top ? s_launcher->loaded_mv : s_launcher->empty_mv) + noise(s_launcher->noise_mv);
}

void sim_sample(int mv)
{
    // main.c: load_classify(diff, launch_get_state() != LAUNCH_PULLING)
    s_loaded = load_classify(mv, !s_pulling);
    // launch.c launch_update(): a fresh load arms the next launch (LAUNCH_LOADED),
    // unloading again before it disarms it (LAUNCH_EMPTY); during a launch neither
    if (!s_pulling) {
        if (s_loaded && !s_was_loaded) {
            s_armed = true;
        } else if (!s_loaded && s_was_loaded) {
            s_armed = false;
        }
    }
    s_was_loaded = s_loaded;
    // main.c: on another launcher, launch_new_launcher(): loaded on the old one's
    // settings and on the new one's is still a new load
    launcher_t launcher = load_launcher();
    if (launcher != s_prev_launcher) {
        s_prev_launcher = launcher;
        s_was_loaded = false;
    }
    fake_clock_advance(s_loaded || s_pulling ? SIM_FAST_US : SIM_POLL_US);
}

static void run(int64_t us)
{
    int64_t end = fake_clock_now() + us;
    while (fake_clock_now() < end) {
        sim_sample(reading());
    }
}

void sim_reset(int black_empty, int black_loaded, int white_empty, int white_loaded, const vlauncher_t *launcher,
               uint32_t seed)
{
    fake_reset();
    fake_set_cal(LAUNCHER_BLACK, black_empty, black_loaded, true);
    fake_set_cal(LAUNCHER_WHITE, white_empty, white_loaded, true);
    load_classifier_restart();
    s_launcher = launcher;
    s_top = false;
    s_pulling = false;
    s_armed = false;
    s_was_loaded = false;
    s_loaded = false;
    s_launches = 0;
    s_prev_launcher = load_launcher();
    s_rng = seed ? seed : SIM_SEED_DEFAULT;
}

void sim_use(const vlauncher_t *launcher)
{
    s_launcher = launcher;
}

void sim_idle(int64_t us)
{
    s_top = false;
    run(us);
}

void sim_load(int64_t us)
{
    s_top = true;
    run(us);
}

bool sim_launch(void)
{
    if (!s_armed) {
        return false;
    }
    s_armed = false;
    s_pulling = true;
    s_top = true;
    run(SIM_RELEASE_US);
    s_top = false;
    run(SIM_SPIN_US);
    s_pulling = false;
    // main.c: the state machine went to LAUNCH_RESULT
    load_launch_ended();
    s_launches++;
    return true;
}

void sim_mode_change(void)
{
    load_classifier_reset(false);
    s_pulling = false;
    s_armed = false;
    s_was_loaded = false;
    s_loaded = false;
}

bool sim_armed(void)
{
    return s_armed;
}

bool sim_loaded(void)
{
    return s_loaded;
}

int sim_launches(void)
{
    return s_launches;
}
