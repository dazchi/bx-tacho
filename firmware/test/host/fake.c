// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Darren Chi
#include "fake.h"

#include <string.h>
#include "esp_timer.h"

// Events kept for the checks; older ones are dropped (counted still)
#define FAKE_EVENTS_MAX 256

typedef struct {
    ev_type_t type;
    int a, b, c, d;
} fake_event_t;

static launcher_cal_t s_cal[LAUNCHER_COUNT];
static bool s_auto_white = true;
static int64_t s_now_us;
static fake_event_t s_events[FAKE_EVENTS_MAX];
static int s_event_count;

// ---- what load_classify.c calls

launcher_cal_t *settings_cal(launcher_t launcher)
{
    return &s_cal[launcher == LAUNCHER_WHITE ? LAUNCHER_WHITE : LAUNCHER_BLACK];
}

const char *launcher_name(launcher_t launcher)
{
    return launcher == LAUNCHER_WHITE ? "white" : "black";
}

bool settings_auto_white(void)
{
    return s_auto_white;
}

int64_t esp_timer_get_time(void)
{
    return s_now_us;
}

void eventlog_add_ext(ev_type_t type, int a, int b, int c, int d, uint8_t flags)
{
    (void)flags;
    s_events[s_event_count % FAKE_EVENTS_MAX] = (fake_event_t){type, a, b, c, d};
    s_event_count++;
}

void eventlog_add(ev_type_t type, int a, int b)
{
    eventlog_add_ext(type, a, b, 0, 0, 0);
}

// ---- the tests' side

void fake_set_cal(launcher_t launcher, int empty_mv, int loaded_mv, bool valid)
{
    launcher_cal_t *c = settings_cal(launcher);
    c->load_empty = (int16_t)empty_mv;
    c->load_loaded = (int16_t)loaded_mv;
    c->load_cal_valid = valid;
}

void fake_set_auto_white(bool on)
{
    s_auto_white = on;
}

void fake_clock_set(int64_t us)
{
    s_now_us = us;
}

void fake_clock_advance(int64_t us)
{
    s_now_us += us;
}

int64_t fake_clock_now(void)
{
    return s_now_us;
}

int fake_events(ev_type_t type, int d)
{
    int n = 0;
    int kept = s_event_count < FAKE_EVENTS_MAX ? s_event_count : FAKE_EVENTS_MAX;
    for (int i = 0; i < kept; i++) {
        const fake_event_t *e = &s_events[i];
        if (e->type == type && (d == FAKE_ANY_D || e->d == d)) {
            n++;
        }
    }
    return n;
}

bool fake_last_event(ev_type_t type, int *a, int *b, int *c, int *d)
{
    int kept = s_event_count < FAKE_EVENTS_MAX ? s_event_count : FAKE_EVENTS_MAX;
    for (int i = s_event_count - 1; i >= s_event_count - kept; i--) {
        const fake_event_t *e = &s_events[i % FAKE_EVENTS_MAX];
        if (e->type == type) {
            *a = e->a;
            *b = e->b;
            *c = e->c;
            *d = e->d;
            return true;
        }
    }
    return false;
}

void fake_events_clear(void)
{
    s_event_count = 0;
}

void fake_reset(void)
{
    memset(s_cal, 0, sizeof(s_cal));
    s_auto_white = true;
    s_now_us = 0;
    fake_events_clear();
}
