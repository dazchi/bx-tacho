#include "cli.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app.h"
#include "bbp.h"
#include "driver/usb_serial_jtag.h"
#include "esp_app_desc.h"
#include "esp_console.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "eventlog.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "load.h"
#include "rpm.h"
#include "settings.h"
#include "vbat_cal.h"

static void print_vbat_cal(const vbat_cal_t *c);

static bool s_have_empty;
static bool s_have_loaded;

static bool require_diag(void)
{
    if (app_get_mode() != APP_MODE_DIAG) {
        printf("Run 'diag on' first (pauses the launch state machine).\n");
        return false;
    }
    return true;
}

static const char *src_name(power_src_t src)
{
    switch (src) {
    case POWER_SRC_CHARGING: return "USB, charging";
    case POWER_SRC_FULL:     return "USB, charged";
    default:                 return "battery";
    }
}

static int cmd_status(int argc, char **argv)
{
    app_snapshot_t s;
    app_get_snapshot(&s);
    const settings_t *cfg = settings_get();

    const esp_app_desc_t *app = esp_app_get_description();
    printf("firmware    : %s v%s, ESP-IDF %s\n", app->project_name, app->version, app->idf_ver);
    printf("mode        : %s\n", app_mode_name(s.mode));
    printf("boot        : #%lu, reset reason %s, light sleep on battery %s\n",
           (unsigned long)eventlog_boot_count(), eventlog_reset_reason_name(esp_reset_reason()),
           settings_light_sleep() ? "on" : "off");
    printf("state       : %s\n", launch_state_name(s.state));
    printf("power       : %s, VBAT %d mV (%s), %d%%\n", src_name(s.batt.src), s.batt.vbat_mv,
           s.batt.vbat_valid ? "valid" : "not trusted while charging", s.batt.percent);
    printf("usb         : VBUS %s, host %s\n", battery_usb_present() ? "yes" : "no",
           usb_serial_jtag_is_connected() ? "connected" : "none");
    printf("rpm channel : %s, threshold %d mV, %u edges/rev, %lu edges dropped, %lu masked by load flashes\n",
           rpm_is_powered() ? "on" : "off", rpm_get_threshold_mv(), cfg->edges_per_rev,
           (unsigned long)rpm_overflow_count(), (unsigned long)rpm_masked_count());
    if (cfg->load_cal_valid) {
        printf("load        : %s, %d mV (%d%%), empty %d mV, loaded %d mV\n",
               s.loaded ? "LOADED" : "empty", s.load_diff_mv, s.load_pct,
               cfg->load_empty, cfg->load_loaded);
    } else {
        printf("load        : not calibrated (RPM stays armed). See 'cal load'.\n");
    }
    if (s.have_result) {
        printf("last launch : peak %d rpm, max %d rpm, %d revs, %d ms\n",
               s.result.peak_rpm, s.result.max_rpm, s.result.revolutions, s.result.duration_ms);
        printf("rejected    : %d edges (max-rpm %d, relative %d)\n",
               s.result.rejected_max_rpm + s.result.rejected_relative,
               s.result.rejected_max_rpm, s.result.rejected_relative);
    }
    if (s.mode == APP_MODE_METER) {
        printf("meter       : avg %d rpm, inst %d, min %d, max %d, %d revs in the last 1 s\n",
               s.meter.avg_rpm, s.meter.inst_rpm, s.meter.min_rpm, s.meter.max_rpm, s.meter.revs);
    }
    if (vbat_cal_valid()) {
        printf("vbat cal    : ");
        print_vbat_cal(vbat_cal_get());
    } else {
        printf("vbat cal    : none (nominal divider ratio). See 'cal vbat start'.\n");
    }
    bbp_info_t b;
    bbp_get_info(&b);
    printf("battle pass : BLE %s, %d shots in history, %lu total, max %u\n",
           !b.radio_on ? "off" : (b.subscribed ? "app listening" : (b.connected ? "connected" : "advertising")),
           b.count, (unsigned long)b.total, b.max_sp);
    printf("idle        : %lu ms\n", (unsigned long)s.idle_ms);
    return 0;
}

static int cmd_diag(int argc, char **argv)
{
    if (argc != 2 || (strcmp(argv[1], "on") && strcmp(argv[1], "off"))) {
        printf("usage: diag on|off\n");
        return 1;
    }
    app_set_mode(strcmp(argv[1], "on") == 0 ? APP_MODE_DIAG : APP_MODE_RUN);
    // Let the sensor task release the hardware before returning
    vTaskDelay(pdMS_TO_TICKS(50));
    printf("diag %s\n", argv[1]);
    return 0;
}

static void print_meter_line(int64_t t0_us)
{
    app_snapshot_t s;
    app_get_snapshot(&s);
    const meter_stats_t *m = &s.meter;
    printf("%lld,%d,%d,%d,%d,%d,%lu,%d,%d\n", (esp_timer_get_time() - t0_us) / 1000,
           m->avg_rpm, m->inst_rpm, m->min_rpm, m->max_rpm, m->revs,
           (unsigned long)m->edges, m->rejected_max_rpm, m->rejected_relative);
}

static int cmd_meter(int argc, char **argv)
{
    if (argc == 2 && (!strcmp(argv[1], "on") || !strcmp(argv[1], "off"))) {
        bool on = !strcmp(argv[1], "on");
        app_set_mode(on ? APP_MODE_METER : APP_MODE_RUN);
        vTaskDelay(pdMS_TO_TICKS(50));
        printf("meter %s%s\n", argv[1], on ? " (RPM channel on, display shows live RPM)" : "");
        return 0;
    }
    if (app_get_mode() != APP_MODE_METER) {
        printf("Run 'meter on' first.\n");
        return 1;
    }

    printf("t_ms,avg_rpm,inst_rpm,min_rpm,max_rpm,revs,edges,rej_max_rpm,rej_relative\n");
    int64_t t0 = esp_timer_get_time();
    if (argc == 1) {
        print_meter_line(t0);
        return 0;
    }
    if (strcmp(argv[1], "log") || argc > 4) {
        printf("usage: meter on|off | meter | meter log [seconds] [interval_ms]\n");
        return 1;
    }
    int seconds = argc >= 3 ? atoi(argv[2]) : 10;
    int interval_ms = argc >= 4 ? atoi(argv[3]) : 500;
    if (seconds < 1 || seconds > 3600 || interval_ms < 50) {
        printf("seconds 1-3600, interval_ms >= 50\n");
        return 1;
    }
    for (int64_t end = t0 + seconds * 1000000LL; esp_timer_get_time() < end;) {
        print_meter_line(t0);
        vTaskDelay(pdMS_TO_TICKS(interval_ms));
    }
    return 0;
}

static int rpm_scan(int seconds, bool apply)
{
    bool was_on = rpm_is_powered();
    rpm_power(true);

    printf("Sampling RPM_SENSE for %d s. Turn the launcher slowly so the sensor sees black and white...\n",
           seconds);
    int lo = INT_MAX;
    int hi = INT_MIN;
    for (int i = 0; i < seconds * 1000; i++) {
        int mv = rpm_read_sense_mv(1);
        if (mv >= 0) {
            lo = mv < lo ? mv : lo;
            hi = mv > hi ? mv : hi;
        }
        vTaskDelay(1);
    }
    if (!was_on) {
        rpm_power(false);
    }

    if (lo > hi) {
        printf("ADC read failed\n");
        return 1;
    }
    int mid = (lo + hi) / 2;
    printf("min %d mV, max %d mV, contrast %d mV, midpoint %d mV\n", lo, hi, hi - lo, mid);
    if (hi - lo < 200) {
        printf("WARNING: contrast below 200 mV. Move the sensor closer or adjust R13/R15.\n");
    }
    if (mid > RPM_THR_MAX_MV) {
        printf("Midpoint is above the comparator's %d mV limit; it will be capped.\n", RPM_THR_MAX_MV);
    }
    if (apply) {
        rpm_set_threshold_mv(mid);
        settings_get()->rpm_thr_mv = (uint16_t)rpm_get_threshold_mv();
        settings_save();
        printf("threshold set to %d mV and saved\n", rpm_get_threshold_mv());
    }
    return 0;
}

static int cmd_rpm(int argc, char **argv)
{
    if (argc < 2) {
        goto usage;
    }
    if (!strcmp(argv[1], "on") || !strcmp(argv[1], "off")) {
        if (!require_diag()) {
            return 1;
        }
        rpm_power(!strcmp(argv[1], "on"));
        printf("rpm channel %s\n", argv[1]);
        return 0;
    }
    if (!strcmp(argv[1], "thr")) {
        if (argc == 3) {
            rpm_set_threshold_mv(atoi(argv[2]));
            settings_get()->rpm_thr_mv = (uint16_t)rpm_get_threshold_mv();
            settings_save();
        }
        printf("threshold %d mV\n", rpm_get_threshold_mv());
        return 0;
    }
    if (!strcmp(argv[1], "sense")) {
        if (!rpm_is_powered()) {
            printf("RPM channel is off; RPM_SENSE is only valid while powered ('rpm on' in diag mode)\n");
            return 1;
        }
        int n = argc == 3 ? atoi(argv[2]) : 10;
        for (int i = 0; i < n; i++) {
            printf("RPM_SENSE %d mV\n", rpm_read_sense_mv(16));
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        return 0;
    }
    if (!strcmp(argv[1], "scan")) {
        if (!require_diag()) {
            return 1;
        }
        int seconds = 5;
        bool apply = false;
        for (int i = 2; i < argc; i++) {
            if (!strcmp(argv[i], "apply")) {
                apply = true;
            } else {
                seconds = atoi(argv[i]);
            }
        }
        if (seconds < 1 || seconds > 60) {
            seconds = 5;
        }
        return rpm_scan(seconds, apply);
    }
usage:
    printf("usage: rpm on|off | thr [mV] | sense [count] | scan [seconds] [apply]\n");
    return 1;
}

static int average_load(void)
{
    int sum = 0;
    int n = 0;
    for (int i = 0; i < 16; i++) {
        int d = load_measure(8);
        if (d != INT_MIN) {
            sum += d;
            n++;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return n ? sum / n : INT_MIN;
}

static int cmd_load(int argc, char **argv)
{
    if (argc == 3 && !strcmp(argv[1], "led") && (!strcmp(argv[2], "on") || !strcmp(argv[2], "off"))) {
        // Diag, or meter mode (which never drives the load LED) to test it at speed
        app_mode_t mode = app_get_mode();
        if (mode != APP_MODE_DIAG && mode != APP_MODE_METER) {
            printf("Run 'diag on' or 'meter on' first.\n");
            return 1;
        }
        load_led_force(!strcmp(argv[2], "on"));
        printf("load IR LED held %s%s\n", argv[2],
               !strcmp(argv[2], "on") ? " (as while a top is loaded); load readings paused" : "");
        return 0;
    }
    int n = argc == 2 ? atoi(argv[1]) : 10;
    for (int i = 0; i < n; i++) {
        printf("LOAD_SENSE (LED on - off) %d mV\n", load_measure(8));
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    return 0;
}

// ---- VBAT calibration (production) -------------------------------------------

#define VBAT_POINT_SETTLE_MS   300   // R3/R9 + C5 settle in ~24 ms; allow for the supply too
#define VBAT_POINT_MAX_SPREAD  6     // mV at the pin between 8 group averages
#define VBAT_POINT_PLAUSIBLE   400   // max |entered - nominal reading| (mV), catches typos
#define VBAT_POINT_MIN_GAP     100   // between calibration points (mV)

static void print_vbat_cal_procedure(void)
{
    printf("VBAT calibration. Setup:\n"
           "  - Power the board ONLY from the supply on J1 (+ to pad 1 VBAT+, - to pad 2 GND),\n"
           "    power switch ON, current limit about 300 mA. No battery connected.\n"
           "  - Connect USB data-only (VBUS cut), otherwise the charger drives VBAT.\n"
           "  - Measure the voltage at the J1 pads with a multimeter (not the supply display).\n"
           "  - Stay within %d-%d mV.\n"
           "Steps:\n"
           "  1. For each point: set the supply, read the multimeter, enter 'cal vbat point <mV>'.\n"
           "     Suggested points: 3300, 3700, 4000, 4300 mV (at least 2, spanning >= 500 mV).\n"
           "  2. 'cal vbat save' fits and stores the calibration.\n"
           "  3. 'cal vbat verify <mV>' at a different voltage (e.g. 3600) checks it (+-%d mV).\n"
           "  4. 'cal vbat stop' returns to normal operation.\n",
           VBAT_CAL_MIN_MV, VBAT_CAL_MAX_MV, VBAT_CAL_VERIFY_TOL_MV);
}

static bool vbat_cal_usb_power_check(void)
{
    if (battery_usb_present()) {
        printf("USB power (VBUS) detected: the charger would drive VBAT and spoil the reading.\n"
               "Use a data-only USB connection and power the board from the supply on J1.\n");
        return false;
    }
    return true;
}

// Average of 8 groups of 16 readings at the VBAT_SENSE pin; *spread = max - min of the groups
static int measure_vbat_pin(int *spread)
{
    vTaskDelay(pdMS_TO_TICKS(VBAT_POINT_SETTLE_MS));
    int lo = INT_MAX;
    int hi = INT_MIN;
    int sum = 0;
    for (int g = 0; g < 8; g++) {
        int mv = battery_read_pin_mv(16);
        if (mv < 0) {
            return -1;
        }
        sum += mv;
        lo = mv < lo ? mv : lo;
        hi = mv > hi ? mv : hi;
        vTaskDelay(pdMS_TO_TICKS(25));
    }
    *spread = hi - lo;
    return (sum + 4) / 8;
}

static void print_vbat_cal(const vbat_cal_t *c)
{
    printf("gain %ld.%06ld, offset %ld mV, %u points over %d-%d mV, max fit error %d mV\n",
           (long)(c->gain_ppm / 1000000), (long)(c->gain_ppm % 1000000), (long)c->offset_mv,
           c->points, c->min_mv, c->max_mv, c->max_err_mv);
}

static int cmd_cal_vbat(int argc, char **argv)
{
    const char *sub = argc >= 2 ? argv[1] : "";

    if (!strcmp(sub, "show") && argc == 2) {
        if (vbat_cal_valid()) {
            printf("stored VBAT calibration: ");
            print_vbat_cal(vbat_cal_get());
        } else {
            printf("no VBAT calibration stored (readings use the nominal divider ratio x2)\n");
        }
        for (int i = 0; i < vbat_cal_session_points(); i++) {
            int pin, vbat;
            vbat_cal_session_point(i, &pin, &vbat);
            printf("  session point %d: VBAT %d mV, pin %d mV\n", i + 1, vbat, pin);
        }
        return 0;
    }
    if (!strcmp(sub, "start") && argc == 2) {
        if (!vbat_cal_usb_power_check()) {
            return 1;
        }
        vbat_cal_session_begin();
        app_set_mode(APP_MODE_VBATCAL);
        vTaskDelay(pdMS_TO_TICKS(50));
        print_vbat_cal_procedure();
        return 0;
    }
    if (!strcmp(sub, "stop") && argc == 2) {
        if (app_get_mode() != APP_MODE_VBATCAL) {
            printf("Not in VBAT calibration mode.\n");
            return 1;
        }
        app_set_mode(APP_MODE_RUN);
        vTaskDelay(pdMS_TO_TICKS(50));
        printf("VBAT calibration mode ended\n");
        return 0;
    }

    if (app_get_mode() != APP_MODE_VBATCAL) {
        printf("Run 'cal vbat start' first.\n");
        return 1;
    }
    if (!vbat_cal_usb_power_check()) {
        return 1;
    }

    if (!strcmp(sub, "point") && argc == 3) {
        int vbat = atoi(argv[2]);
        if (vbat < VBAT_CAL_MIN_MV || vbat > VBAT_CAL_MAX_MV) {
            printf("Enter the measured voltage in mV, %d-%d.\n", VBAT_CAL_MIN_MV, VBAT_CAL_MAX_MV);
            return 1;
        }
        for (int i = 0; i < vbat_cal_session_points(); i++) {
            int pin, have;
            vbat_cal_session_point(i, &pin, &have);
            if (abs(have - vbat) < VBAT_POINT_MIN_GAP) {
                printf("Already have a point at %d mV; use points at least %d mV apart "
                       "('cal vbat start' starts over).\n", have, VBAT_POINT_MIN_GAP);
                return 1;
            }
        }
        int spread;
        int pin = measure_vbat_pin(&spread);
        if (pin < 0) {
            printf("ADC read failed\n");
            return 1;
        }
        if (spread > VBAT_POINT_MAX_SPREAD) {
            printf("Reading unstable (%d mV spread at the pin). Check the supply and connections.\n", spread);
            return 1;
        }
        int nominal = vbat_cal_nominal(pin);
        if (abs(nominal - vbat) > VBAT_POINT_PLAUSIBLE) {
            printf("Measured %d mV nominal vs %d mV entered: too far apart. Check the value and wiring.\n",
                   nominal, vbat);
            return 1;
        }
        if (vbat_cal_session_add(pin, vbat) != ESP_OK) {
            printf("Maximum of %d points reached. 'cal vbat save' or 'cal vbat start' to start over.\n",
                   VBAT_CAL_MAX_POINTS);
            return 1;
        }
        printf("point %d: VBAT %d mV, pin %d mV (nominal reading %d mV, %+d mV)\n",
               vbat_cal_session_points(), vbat, pin, nominal, nominal - vbat);
        return 0;
    }
    if (!strcmp(sub, "save") && argc == 2) {
        vbat_cal_t cal = {0};
        int resid[VBAT_CAL_MAX_POINTS] = {0};
        const char *why = vbat_cal_session_fit(&cal, resid);
        // cal.points is set whenever a line was fitted, also if it was then rejected
        if (cal.points > 0) {
            printf("  VBAT mV   pin mV   fit error\n");
            for (int i = 0; i < vbat_cal_session_points(); i++) {
                int pin, vbat;
                vbat_cal_session_point(i, &pin, &vbat);
                printf("  %7d  %7d  %+6d mV\n", vbat, pin, resid[i]);
            }
        }
        if (why) {
            printf("Not saved: %s.\n", why);
            return 1;
        }
        if (vbat_cal_save(&cal) != ESP_OK) {
            printf("Saving failed (factory partition unavailable?).\n");
            return 1;
        }
        printf("Saved: ");
        print_vbat_cal(&cal);
        printf("Now check it: set another voltage and run 'cal vbat verify <mV>'.\n");
        return 0;
    }
    if (!strcmp(sub, "verify") && argc == 3) {
        if (!vbat_cal_valid()) {
            printf("No calibration stored yet ('cal vbat save').\n");
            return 1;
        }
        int actual = atoi(argv[2]);
        if (actual < VBAT_CAL_MIN_MV || actual > VBAT_CAL_MAX_MV) {
            printf("Enter the measured voltage in mV, %d-%d.\n", VBAT_CAL_MIN_MV, VBAT_CAL_MAX_MV);
            return 1;
        }
        int spread;
        int pin = measure_vbat_pin(&spread);
        if (pin < 0) {
            printf("ADC read failed\n");
            return 1;
        }
        int measured = vbat_cal_apply(pin);
        int err = measured - actual;
        bool pass = abs(err) <= VBAT_CAL_VERIFY_TOL_MV && spread <= VBAT_POINT_MAX_SPREAD;
        printf("VBAT %d mV measured vs %d mV actual: %+d mV (nominal would be %+d mV)%s -> %s\n",
               measured, actual, err, vbat_cal_nominal(pin) - actual,
               spread > VBAT_POINT_MAX_SPREAD ? ", reading unstable" : "", pass ? "PASS" : "FAIL");
        return pass ? 0 : 1;
    }

    printf("usage: cal vbat start | point <mV> | save | verify <mV> | show | stop\n");
    return 1;
}

static int cmd_cal(int argc, char **argv)
{
    settings_t *cfg = settings_get();

    if (argc >= 2 && !strcmp(argv[1], "vbat")) {
        return cmd_cal_vbat(argc - 1, argv + 1);
    }

    if (argc == 3 && !strcmp(argv[1], "load")) {
        if (!strcmp(argv[2], "clear")) {
            cfg->load_cal_valid = false;
            s_have_empty = s_have_loaded = false;
            settings_save();
            printf("load calibration cleared\n");
            return 0;
        }
        bool empty = !strcmp(argv[2], "empty");
        if (!empty && strcmp(argv[2], "loaded")) {
            goto usage;
        }
        int level = average_load();
        if (level == INT_MIN) {
            printf("ADC read failed\n");
            return 1;
        }
        if (empty) {
            cfg->load_empty = (int16_t)level;
            s_have_empty = true;
        } else {
            cfg->load_loaded = (int16_t)level;
            s_have_loaded = true;
        }
        printf("%s level %d mV\n", argv[2], level);

        if ((s_have_empty && s_have_loaded) || cfg->load_cal_valid) {
            int span = abs(cfg->load_loaded - cfg->load_empty);
            if (span < LOAD_MIN_CONTRAST_MV) {
                printf("Contrast only %d mV (need %d). Not saved as valid.\n", span, LOAD_MIN_CONTRAST_MV);
                cfg->load_cal_valid = false;
            } else {
                cfg->load_cal_valid = true;
                printf("Calibration valid, contrast %d mV.\n", span);
            }
        } else {
            printf("Now run 'cal load %s'.\n", empty ? "loaded" : "empty");
        }
        settings_save();
        return 0;
    }
usage:
    printf("usage: cal load empty|loaded|clear\n"
           "       cal vbat start | point <mV> | save | verify <mV> | show | stop\n");
    return 1;
}

static int cmd_cfg(int argc, char **argv)
{
    settings_t *cfg = settings_get();

    if (argc == 3 && !strcmp(argv[1], "edges")) {
        int n = atoi(argv[2]);
        if (n < 1 || n > 16) {
            printf("edges must be 1-16\n");
            return 1;
        }
        cfg->edges_per_rev = (uint8_t)n;
        settings_save();
        printf("edges per revolution: %d\n", n);
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "reset")) {
        settings_reset();
        settings_save();
        rpm_set_threshold_mv(cfg->rpm_thr_mv);
        s_have_empty = s_have_loaded = false;
        printf("settings reset to defaults\n");
        return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "lightsleep") && (!strcmp(argv[2], "on") || !strcmp(argv[2], "off"))) {
        bool on = !strcmp(argv[2], "on");
        settings_set_light_sleep(on);
        app_set_light_sleep(on);
        printf("light sleep on battery: %s (saved)\n", argv[2]);
        return 0;
    }
    printf("usage: cfg edges <1-16> | cfg lightsleep on|off | cfg reset\n");
    return 1;
}

// Microseconds as milliseconds with one decimal, right-aligned in 7 characters
static void print_ms(uint32_t us)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "%lu.%lu", (unsigned long)(us / 1000), (unsigned long)(us % 1000 / 100));
    printf("%7s", buf);
}

// Last launch, edge by edge: interval, RPM over one revolution, the release
// seen by the load sensor and the edges the chatter filter dropped
static int cmd_launch(int argc, char **argv)
{
    static uint32_t edges[200];
    static launch_trace_t info;
    int max = argc > 1 ? atoi(argv[1]) : 60;
    if (argc > 2 || max < 2 || max > (int)(sizeof(edges) / sizeof(edges[0]))) {
        printf("usage: launch [edges 2-200, default 60]\n");
        return 1;
    }
    if (!launch_get_trace(edges, max, &info)) {
        printf("No finished launch yet.\n");
        return 1;
    }
    int epr = settings_get()->edges_per_rev;
    int n = info.num_edges < max ? info.num_edges : max;
    uint32_t t0 = edges[0];

    printf("%d edges (showing %d), %d edge(s)/rev, release %s", info.num_edges, n, epr,
           info.release_seen ? "at " : "not seen by the load sensor");
    if (info.release_seen) {
        uint32_t rel = info.release_us - t0;
        printf("+%lu.%lu ms", (unsigned long)(rel / 1000), (unsigned long)(rel % 1000 / 100));
    }
    printf(", %d edge(s) dropped by the filter", info.num_rejected);
    if (info.lock_recoveries > 0) {
        printf(", filter lock recovered %d time(s)", info.lock_recoveries);
    }
    printf("\n");
    printf("  #   t (ms)  interval (us)  rpm (1 rev)\n");

    int r = 0;   // next dropped edge to print
    bool release_printed = !info.release_seen;
    for (int i = 0; i < n; i++) {
        // Dropped edges and the release, in time order before this edge
        while (r < info.num_rejected && (int32_t)(info.rejected_us[r] - edges[i]) < 0) {
            printf("     ");
            print_ms(info.rejected_us[r] - t0);
            printf("   -- dropped (%s)\n", info.rejected_max_rpm[r] ? "max rpm" : "too soon");
            r++;
        }
        if (!release_printed && (int32_t)(info.release_us - edges[i]) < 0) {
            printf("     ");
            print_ms(info.release_us - t0);
            printf("   -- release (load sensor)\n");
            release_printed = true;
        }
        printf("%3d  ", i);
        print_ms(edges[i] - t0);
        if (i > 0) {
            printf("  %13lu", (unsigned long)(edges[i] - edges[i - 1]));
        }
        if (i >= epr) {
            printf("  %11lu", (unsigned long)(60000000ULL / (edges[i] - edges[i - epr])));
        }
        printf("\n");
    }
    return 0;
}

static int cmd_ble(int argc, char **argv)
{
    if (argc == 2 && (!strcmp(argv[1], "on") || !strcmp(argv[1], "off"))) {
        bool on = !strcmp(argv[1], "on");
        if (on == settings_ble()) {
            printf("BLE is already %s\n", argv[1]);
            return 0;
        }
        settings_set_ble(on);
        printf("BLE %s (saved). Restarting to apply...\n", argv[1]);
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_restart();
    }
    if (argc != 1) {
        printf("usage: ble | ble on|off\n");
        return 1;
    }

    bbp_info_t b;
    bbp_get_info(&b);
    printf("BLE: %s", b.radio_on ? "on, " BBP_DEVICE_NAME : "off");
    if (b.radio_on) {
        printf(", %s", b.subscribed ? "app listening" : (b.connected ? "connected" : "advertising"));
    }
    printf("\n");
    return 0;
}

// The Battle Pass shot history that apps read (kept with BLE off too)
static int cmd_history(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "clear")) {
        bbp_clear_history();
        printf("Shot history cleared: last %d shots, total count, max and latest curve\n",
               BBP_HISTORY_LEN);
        return 0;
    }
    if (argc != 1) {
        printf("usage: history | history clear\n");
        return 1;
    }

    bbp_info_t b;
    bbp_get_info(&b);
    printf("shots   : %d in history, %lu total, max %u\n", b.count, (unsigned long)b.total, b.max_sp);
    for (int i = 0; i < b.count; i++) {
        // Ten per line, oldest first
        printf("%s%5u", i % 10 ? " " : (i ? "\n          " : "history : "), b.sp[i]);
    }
    if (b.count > 0) {
        printf("\n");
    }
    return 0;
}

static int cmd_sleep(int argc, char **argv)
{
    printf("Deep sleep in %d s. Unplug USB now, otherwise sleep is cancelled.\n"
           "Wake up later by plugging in USB or cycling the power switch.\n", APP_SLEEP_COUNTDOWN_S);
    if (!app_enter_deep_sleep(APP_SLEEP_COMMAND)) {
        printf("USB still connected: deep sleep cancelled.\n");
        return 1;
    }
    return 0;
}

static int cmd_events(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "clear")) {
        eventlog_clear();
        printf("event log cleared\n");
        return 0;
    }
    if (argc != 1) {
        printf("usage: events | events clear\n");
        return 1;
    }
    eventlog_print();
    return 0;
}

esp_err_t cli_start(void)
{
    const esp_console_cmd_t cmds[] = {
        {.command = "status", .help = "Show sensors, battery, calibration and the last launch", .func = cmd_status},
        {.command = "diag", .help = "diag on|off: pause the state machine for manual sensor control", .func = cmd_diag},
        {.command = "meter", .help = "meter on|off | meter | meter log [seconds] [interval_ms]: continuous RPM (CSV)", .func = cmd_meter},
        {.command = "rpm", .help = "rpm on|off | thr [mV] | sense [count] | scan [seconds] [apply]", .func = cmd_rpm},
        {.command = "load", .help = "load [count]: print ambient-cancelled LOAD_SENSE readings; load led on|off: hold the load LED (diag or meter mode)", .func = cmd_load},
        {.command = "cal", .help = "cal load empty|loaded|clear: load sensor; cal vbat start|point <mV>|save|verify <mV>|show|stop: production VBAT calibration (kept by 'cfg reset')", .func = cmd_cal},
        {.command = "cfg", .help = "cfg edges <n> | cfg lightsleep on|off | cfg reset", .func = cmd_cfg},
        {.command = "launch", .help = "launch [n]: last launch edge by edge (first n edges, default 60), with the release and dropped edges", .func = cmd_launch},
        {.command = "ble", .help = "ble | ble on|off: Battle Pass emulation over BLE (on|off restarts)", .func = cmd_ble},
        {.command = "history", .help = "history | history clear: shot history sent to Battle Pass apps (last 50, total, max, latest curve)", .func = cmd_history},
        {.command = "sleep", .help = "Enter deep sleep now", .func = cmd_sleep},
        {.command = "events", .help = "events | events clear: power/sleep event log (survives deep sleep and resets)", .func = cmd_events},
    };

    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_cfg.prompt = "bxt>";
    ESP_ERROR_CHECK(esp_console_new_repl_stdio(&repl_cfg, &repl));

    esp_console_register_help_command();
    for (int i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        ESP_ERROR_CHECK(esp_console_cmd_register(&cmds[i]));
    }
    return esp_console_start_repl(repl);
}
