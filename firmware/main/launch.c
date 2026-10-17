#include "launch.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "rpm.h"
#include "settings.h"

static const char *TAG = "launch";

// Launch start: 3 edges within 200 ms. Launch end: 250 ms without an edge.
#define START_EDGES       3
#define START_WINDOW_US   200000
#define END_GAP_US        250000

// A launch whose last revolution is this much slower than its fastest one
// was a pull that faded out rather than a release.
#define SLOWDOWN_RATIO    2

#define MAX_EDGES         4096

static launch_state_t s_state = LAUNCH_EMPTY;

static uint32_t s_edges[MAX_EDGES];
static int s_num_edges;

static uint32_t s_window[START_EDGES];
static int s_window_len;

static uint32_t s_last_edge;
static bool s_have_last_edge;

static bool s_was_loaded;
static bool s_release_seen;
static uint32_t s_release_us;

static launch_result_t s_result;
static bool s_have_result;

static inline bool time_before_eq(uint32_t a, uint32_t b)
{
    return (int32_t)(a - b) <= 0;
}

static int period_to_rpm(uint32_t period_us)
{
    return period_us ? (int)(60000000ULL / period_us) : 0;
}

const char *launch_state_name(launch_state_t s)
{
    switch (s) {
    case LAUNCH_EMPTY:   return "EMPTY";
    case LAUNCH_LOADED:  return "LOADED";
    case LAUNCH_PULLING: return "PULLING";
    case LAUNCH_RESULT:  return "RESULT";
    }
    return "?";
}

static void set_state(launch_state_t s)
{
    if (s != s_state) {
        ESP_LOGI(TAG, "%s -> %s", launch_state_name(s_state), launch_state_name(s));
        s_state = s;
    }
}

void launch_init(void)
{
    s_state = LAUNCH_EMPTY;
    s_window_len = 0;
    s_have_last_edge = false;
}

static void start_launch(void)
{
    s_num_edges = 0;
    for (int i = 0; i < s_window_len; i++) {
        s_edges[s_num_edges++] = s_window[i];
    }
    s_window_len = 0;
    s_release_seen = false;
    set_state(LAUNCH_PULLING);
}

// Returns false if the launch was too short to count
static bool finish_launch(void)
{
    int epr = settings_get()->edges_per_rev;

    // End the window at the release if the load sensor saw one: the launcher
    // mechanism keeps turning briefly after the top has left.
    int n = s_num_edges;
    if (s_release_seen) {
        while (n > 0 && !time_before_eq(s_edges[n - 1], s_release_us)) {
            n--;
        }
    }
    if (n <= epr) {
        return false;
    }

    // Three fastest revolutions; the peak is their median (rejects a single glitch)
    uint32_t best[3] = {UINT32_MAX, UINT32_MAX, UINT32_MAX};
    for (int i = epr; i < n; i++) {
        uint32_t p = s_edges[i] - s_edges[i - epr];
        if (p < best[0]) {
            best[2] = best[1];
            best[1] = best[0];
            best[0] = p;
        } else if (p < best[1]) {
            best[2] = best[1];
            best[1] = p;
        } else if (p < best[2]) {
            best[2] = p;
        }
    }
    int periods = n - epr;
    uint32_t median = periods >= 3 ? best[1] : best[0];
    uint32_t last = s_edges[n - 1] - s_edges[n - 1 - epr];

    s_result = (launch_result_t) {
        .peak_rpm = period_to_rpm(median),
        .max_rpm = period_to_rpm(best[0]),
        .revolutions = (n - 1) / epr,
        .duration_ms = (int)((s_edges[n - 1] - s_edges[0]) / 1000),
        .end = s_release_seen ? LAUNCH_END_RELEASE
             : (last > best[0] * SLOWDOWN_RATIO ? LAUNCH_END_SLOWDOWN : LAUNCH_END_ABRUPT),
    };
    s_have_result = true;

    ESP_LOGI(TAG, "peak %d rpm, max %d rpm, %d revs, %d ms, end=%d",
             s_result.peak_rpm, s_result.max_rpm, s_result.revolutions,
             s_result.duration_ms, s_result.end);
    return true;
}

static void handle_edge(uint32_t t, bool can_start, uint32_t min_gap_us)
{
    // Faster than physically possible: comparator chatter
    if (s_have_last_edge && (t - s_last_edge) < min_gap_us) {
        return;
    }
    s_last_edge = t;
    s_have_last_edge = true;

    if (s_state == LAUNCH_PULLING) {
        if (s_num_edges < MAX_EDGES) {
            s_edges[s_num_edges++] = t;
        }
        return;
    }
    if (!can_start) {
        s_window_len = 0;
        return;
    }

    // Slide the start window
    if (s_window_len == START_EDGES) {
        for (int i = 1; i < START_EDGES; i++) {
            s_window[i - 1] = s_window[i];
        }
        s_window_len--;
    }
    s_window[s_window_len++] = t;
    if (s_window_len == START_EDGES && (t - s_window[0]) <= START_WINDOW_US) {
        start_launch();
    }
}

bool launch_update(bool loaded, bool armed_always)
{
    launch_state_t prev = s_state;
    uint32_t now = (uint32_t)esp_timer_get_time();
    const settings_t *cfg = settings_get();

    // Load transitions
    if (loaded && !s_was_loaded && s_state != LAUNCH_PULLING) {
        set_state(LAUNCH_LOADED);
    } else if (!loaded && s_was_loaded) {
        if (s_state == LAUNCH_PULLING && !s_release_seen) {
            s_release_seen = true;
            s_release_us = now;
        } else if (s_state == LAUNCH_LOADED) {
            set_state(LAUNCH_EMPTY);
        }
    }
    s_was_loaded = loaded;

    bool can_start = armed_always || s_state == LAUNCH_LOADED;
    bool want_power = can_start || loaded || s_state == LAUNCH_PULLING;
    if (want_power != rpm_is_powered()) {
        rpm_power(want_power);
        s_window_len = 0;
        s_have_last_edge = false;
    }

    uint32_t min_gap_us = 60000000UL / ((uint32_t)CONFIG_BXT_MAX_RPM * cfg->edges_per_rev);
    uint32_t edges[32];
    int n;
    while ((n = rpm_take_edges(edges, 32)) > 0) {
        for (int i = 0; i < n; i++) {
            handle_edge(edges[i], can_start, min_gap_us);
        }
    }

    if (s_state == LAUNCH_PULLING && (now - s_last_edge) > END_GAP_US) {
        if (finish_launch()) {
            set_state(LAUNCH_RESULT);
        } else {
            set_state(loaded ? LAUNCH_LOADED : LAUNCH_EMPTY);
        }
    }

    return s_state != prev || s_state == LAUNCH_PULLING;
}

launch_state_t launch_get_state(void)
{
    return s_state;
}

int launch_live_rpm(void)
{
    int epr = settings_get()->edges_per_rev;
    if (s_state != LAUNCH_PULLING || s_num_edges <= epr) {
        return 0;
    }
    return period_to_rpm(s_edges[s_num_edges - 1] - s_edges[s_num_edges - 1 - epr]);
}

bool launch_get_result(launch_result_t *out)
{
    if (s_have_result) {
        *out = s_result;
    }
    return s_have_result;
}
