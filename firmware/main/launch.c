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

// Chatter filter. Besides the absolute floor (CONFIG_BXT_MAX_RPM), an edge is
// dropped when it arrives sooner than CONFIG_BXT_CHATTER_RATIO_PCT of the
// recent edge interval: the disc cannot speed up that much within one
// interval, but chatter at the opposite transition lands about half an
// interval after a real edge. Assumes evenly spaced segments.
#define HIST_LEN            3
// A gap this much longer than the reference is a missed edge (or the disc
// slowing sharply); it is not used as a reference so the filter cannot lock
// onto half speed.
#define MISSED_EDGE_PCT     170

// Meter mode: average over this window; report 0 RPM after this long without an edge
#define METER_WINDOW_US     1000000
#define METER_TIMEOUT_US    2000000

static launch_state_t s_state = LAUNCH_EMPTY;

static uint32_t s_edges[MAX_EDGES];
static int s_num_edges;

static uint32_t s_window[START_EDGES];
static int s_window_len;

static uint32_t s_last_edge;
static bool s_have_last_edge;

static uint32_t s_hist[HIST_LEN];   // recent accepted edge intervals (us)
static int s_hist_len;
static int s_hist_next;
static int s_long_streak;           // consecutive gaps rejected as references
static int s_rejected_max_rpm;      // edges dropped in this launch: faster than CONFIG_BXT_MAX_RPM
static int s_rejected_relative;     // ...or too soon for the recent interval

static uint32_t s_meter_count;      // meter mode: accepted edges, stored in s_edges as a ring

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

static void filter_reset(void)
{
    s_hist_len = 0;
    s_hist_next = 0;
    s_long_streak = 0;
}

// Reference interval for the relative filter, or 0 until enough history.
// min(last, median) follows a speed-up immediately and ignores one long gap.
static uint32_t filter_reference(void)
{
    if (s_hist_len < HIST_LEN) {
        return 0;
    }
    uint32_t a = s_hist[0], b = s_hist[1], c = s_hist[2];
    uint32_t lo = a < b ? (a < c ? a : c) : (b < c ? b : c);
    uint32_t hi = a > b ? (a > c ? a : c) : (b > c ? b : c);
    uint32_t median = a + b + c - lo - hi;
    uint32_t last = s_hist[(s_hist_next + HIST_LEN - 1) % HIST_LEN];
    return last < median ? last : median;
}

static void filter_record(uint32_t gap, uint32_t ref)
{
    if (ref && (uint64_t)gap * 100 > (uint64_t)ref * MISSED_EDGE_PCT) {
        // Likely a missed edge. Only if it keeps happening has the rhythm
        // really changed; then start the history again from here.
        if (++s_long_streak < HIST_LEN) {
            return;
        }
        filter_reset();
    }
    s_long_streak = 0;
    s_hist[s_hist_next] = gap;
    s_hist_next = (s_hist_next + 1) % HIST_LEN;
    if (s_hist_len < HIST_LEN) {
        s_hist_len++;
    }
}

void launch_init(void)
{
    s_state = LAUNCH_EMPTY;
    s_window_len = 0;
    s_have_last_edge = false;
    filter_reset();
}

static void start_launch(void)
{
    s_num_edges = 0;
    for (int i = 0; i < s_window_len; i++) {
        s_edges[s_num_edges++] = s_window[i];
    }
    s_window_len = 0;
    s_release_seen = false;
    s_rejected_max_rpm = 0;
    s_rejected_relative = 0;
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
        .rejected_max_rpm = s_rejected_max_rpm,
        .rejected_relative = s_rejected_relative,
    };
    s_have_result = true;

    ESP_LOGI(TAG, "peak %d rpm, max %d rpm, %d revs, %d ms, end=%d, rejected %d (max-rpm %d, relative %d)",
             s_result.peak_rpm, s_result.max_rpm, s_result.revolutions, s_result.duration_ms,
             s_result.end, s_result.rejected_max_rpm + s_result.rejected_relative,
             s_result.rejected_max_rpm, s_result.rejected_relative);
    return true;
}

static uint32_t min_edge_gap_us(void)
{
    return 60000000UL / ((uint32_t)CONFIG_BXT_MAX_RPM * settings_get()->edges_per_rev);
}

// Chatter filter shared by launches and the meter. Returns false if the edge
// is dropped.
static bool accept_edge(uint32_t t, uint32_t min_gap_us)
{
    if (s_have_last_edge) {
        uint32_t gap = t - s_last_edge;
        if (gap > END_GAP_US) {
            filter_reset();   // the disc stopped; the old rhythm no longer applies
        }
        uint32_t ref = filter_reference();
        uint32_t rel_limit = (uint32_t)((uint64_t)ref * CONFIG_BXT_CHATTER_RATIO_PCT / 100);
        // Faster than physically possible, or too soon after the last edge: chatter
        if (gap < min_gap_us) {
            s_rejected_max_rpm++;
            return false;
        }
        if (gap < rel_limit) {
            s_rejected_relative++;
            return false;
        }
        filter_record(gap, ref);
    }
    s_last_edge = t;
    s_have_last_edge = true;
    return true;
}

static void handle_edge(uint32_t t, bool can_start, uint32_t min_gap_us)
{
    if (!accept_edge(t, min_gap_us)) {
        return;
    }

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
        filter_reset();
    }

    uint32_t min_gap_us = min_edge_gap_us();
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

void launch_meter_start(void)
{
    s_state = LAUNCH_EMPTY;
    s_window_len = 0;
    s_have_last_edge = false;
    filter_reset();
    s_meter_count = 0;
    s_rejected_max_rpm = 0;
    s_rejected_relative = 0;
}

static inline uint32_t meter_edge(uint32_t i)
{
    return s_edges[i % MAX_EDGES];
}

void launch_meter_update(meter_stats_t *out)
{
    uint32_t epr = settings_get()->edges_per_rev;
    uint32_t min_gap_us = min_edge_gap_us();
    uint32_t edges[32];
    int n;
    while ((n = rpm_take_edges(edges, 32)) > 0) {
        for (int i = 0; i < n; i++) {
            if (accept_edge(edges[i], min_gap_us)) {
                s_edges[s_meter_count % MAX_EDGES] = edges[i];
                s_meter_count++;
            }
        }
    }

    *out = (meter_stats_t) {
        .edges = s_meter_count,
        .rejected_max_rpm = s_rejected_max_rpm,
        .rejected_relative = s_rejected_relative,
    };

    // Edges still held in the ring, and how far back from the newest one we can go
    uint32_t held = s_meter_count < MAX_EDGES ? s_meter_count : MAX_EDGES;
    if (held <= epr) {
        return;
    }
    uint32_t newest = s_meter_count - 1;
    uint32_t t_last = meter_edge(newest);
    if ((uint32_t)esp_timer_get_time() - t_last > METER_TIMEOUT_US) {
        return;   // stopped
    }

    // Whole revolutions within the window, but at least one when turning slowly
    uint32_t steps = 0;
    while (steps < held - 1 && t_last - meter_edge(newest - steps - 1) <= METER_WINDOW_US) {
        steps++;
    }
    steps -= steps % epr;
    if (steps == 0) {
        steps = epr;
    }

    uint32_t revs = steps / epr;
    uint32_t span = t_last - meter_edge(newest - steps);
    uint32_t slowest = 0;
    uint32_t fastest = UINT32_MAX;
    for (uint32_t back = 0; back + epr <= steps; back++) {
        uint32_t p = meter_edge(newest - back) - meter_edge(newest - back - epr);
        slowest = p > slowest ? p : slowest;
        fastest = p < fastest ? p : fastest;
    }

    out->revs = (int)revs;
    out->avg_rpm = span ? (int)(60000000ULL * revs / span) : 0;
    out->inst_rpm = period_to_rpm(t_last - meter_edge(newest - epr));
    out->min_rpm = period_to_rpm(slowest);
    out->max_rpm = period_to_rpm(fastest);
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
