// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Darren Chi
#include "cli.h"

#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app.h"
#include "bbp.h"
#include "display.h"
#include "driver/usb_serial_jtag.h"
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_chip_info.h"
#include "esp_efuse.h"
#include "esp_efuse_table.h"
#include "esp_mac.h"
#include "esp_console.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "eventlog.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "linenoise/linenoise.h"
#include "load.h"
#include "rpm.h"
#include "settings.h"
#include "vbat_cal.h"

#define CLI_MAX_ARGS   8
#define CLI_PRINT_BUF  256     // on the stack; longer output is formatted on the heap

static void print_vbat_cal(const vbat_cal_t *c);

// 'cal load' levels measured since boot, per launcher
static bool s_have_empty[LAUNCHER_COUNT];
static bool s_have_loaded[LAUNCHER_COUNT];

// ---- Consoles -----------------------------------------------------------------

// The console running a command in this task; NULL in the USB REPL task
static __thread cli_console_t *t_console;
static SemaphoreHandle_t s_cmd_lock;    // one command at a time, over all consoles
static volatile bool s_usb_stop;

int cli_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    cli_console_t *console = t_console;
    int n;
    if (console == NULL) {
        n = vprintf(fmt, ap);
    } else {
        char buf[CLI_PRINT_BUF];
        va_list again;
        va_copy(again, ap);
        n = vsnprintf(buf, sizeof(buf), fmt, ap);
        if (n >= (int)sizeof(buf)) {
            char *big = malloc(n + 1);
            if (big != NULL) {
                vsnprintf(big, n + 1, fmt, again);
                console->write(big, n);
                free(big);
            } else {
                console->write(buf, sizeof(buf) - 1);
            }
        } else if (n > 0) {
            console->write(buf, n);
        }
        va_end(again);
    }
    va_end(ap);
    return n;
}

bool cli_stop_requested(void)
{
    cli_console_t *console = t_console;
    if (console != NULL) {
        return console->stop;
    }
    // USB: nothing reads the console while a command runs, so look for Ctrl-C
    // here. Anything else typed meanwhile is dropped.
    uint8_t buf[16];
    int n;
    while (!s_usb_stop && usb_serial_jtag_is_driver_installed() &&
           (n = usb_serial_jtag_read_bytes(buf, sizeof(buf), 0)) > 0) {
        if (memchr(buf, 0x03, n) != NULL) {
            s_usb_stop = true;
        }
    }
    return s_usb_stop;
}

bool cli_wait_ms(int ms)
{
    while (ms > 0 && !cli_stop_requested()) {
        int step = ms < 20 ? ms : 20;
        vTaskDelay(pdMS_TO_TICKS(step));
        ms -= step;
    }
    return !cli_stop_requested();
}

static void print_stopped(void)
{
    cli_printf("^C stopped\n");
}

static bool require_diag(void)
{
    if (app_get_mode() != APP_MODE_DIAG) {
        cli_printf("Run 'diag on' first (pauses the launch state machine).\n");
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
    const launcher_cal_t *lcal = settings_cal(s.launcher);

    const esp_app_desc_t *app = esp_app_get_description();
    cli_printf("firmware    : %s v%s, ESP-IDF %s\n", app->project_name, app->version, app->idf_ver);
    cli_printf("mode        : %s\n", app_mode_name(s.mode));
    cli_printf("boot        : #%lu, reset reason %s, light sleep on battery %s\n",
               (unsigned long)eventlog_boot_count(), eventlog_reset_reason_name(esp_reset_reason()),
               settings_light_sleep() ? "on" : "off");
    cli_printf("display     : %s\n", settings_flip() ? "turned 180 degrees ('cfg flip off')" : "normal ('cfg flip on' turns it)");
    cli_printf("state       : %s\n", launch_state_name(s.state));
    cli_printf("power       : %s, VBAT %d mV (%s), %d%%\n", src_name(s.batt.src), s.batt.vbat_mv,
               s.batt.vbat_valid ? "valid" : "not trusted while charging", s.batt.percent);
    cli_printf("usb         : VBUS %s, host %s\n", battery_usb_present() ? "yes" : "no",
               usb_serial_jtag_is_connected() ? "connected" : "none");
    cli_printf("launcher    : %s\n", launcher_name(s.launcher));
    cli_printf("rpm channel : %s, threshold %d mV, %u edges/rev, %lu edges dropped, %lu masked by load flashes, "
               "%lu edge flashes cut short\n",
               rpm_is_powered() ? "on" : "off", rpm_get_threshold_mv(), cfg->edges_per_rev,
               (unsigned long)rpm_overflow_count(), (unsigned long)rpm_masked_count(),
               (unsigned long)load_edge_flash_late_count());
    if (lcal->load_cal_valid) {
        cli_printf("load        : %s, %d mV (%d%%)\n", s.loaded ? "LOADED" : "empty", s.load_diff_mv, s.load_pct);
    } else {
        cli_printf("load        : not calibrated for this launcher (RPM stays armed). See 'cal load'.\n");
    }
    for (int l = 0; l < LAUNCHER_COUNT; l++) {
        const launcher_cal_t *c = settings_cal(l);
        if (c->load_cal_valid) {
            cli_printf("cal %-7s : load empty %d mV, loaded %d mV; rpm threshold %u mV\n", launcher_name(l),
                       c->load_empty, c->load_loaded, c->rpm_thr_mv);
        } else {
            cli_printf("cal %-7s : load not calibrated; rpm threshold %u mV\n", launcher_name(l), c->rpm_thr_mv);
        }
    }
    if (s.have_result) {
        cli_printf("last launch : peak %d rpm, max %d rpm, %d revs, %d ms\n",
                   s.result.peak_rpm, s.result.max_rpm, s.result.revolutions, s.result.duration_ms);
        cli_printf("rejected    : %d edges (max-rpm %d, relative %d)\n",
                   s.result.rejected_max_rpm + s.result.rejected_relative,
                   s.result.rejected_max_rpm, s.result.rejected_relative);
    }
    if (s.mode == APP_MODE_METER) {
        cli_printf("meter       : avg %d rpm, inst %d, min %d, max %d, %d revs in the last 1 s\n",
                   s.meter.avg_rpm, s.meter.inst_rpm, s.meter.min_rpm, s.meter.max_rpm, s.meter.revs);
    }
    if (vbat_cal_valid()) {
        cli_printf("vbat cal    : ");
        print_vbat_cal(vbat_cal_get());
    } else {
        cli_printf("vbat cal    : none (nominal divider ratio). See 'cal vbat start'.\n");
    }
    bbp_info_t b;
    bbp_get_info(&b);
    cli_printf("battle pass : BLE %s, %d shots in history, %lu total, max %u\n",
               !b.radio_on ? "off" : (b.subscribed ? "app listening" : (b.connected ? "connected" : "advertising")),
               b.count, (unsigned long)b.total, b.max_sp);
    cli_printf("idle        : %lu ms\n", (unsigned long)s.idle_ms);
    return 0;
}

static int cmd_diag(int argc, char **argv)
{
    if (argc != 2 || (strcmp(argv[1], "on") && strcmp(argv[1], "off"))) {
        cli_printf("usage: diag on|off\n");
        return 1;
    }
    app_set_mode(strcmp(argv[1], "on") == 0 ? APP_MODE_DIAG : APP_MODE_RUN);
    // Let the sensor task release the hardware before returning
    vTaskDelay(pdMS_TO_TICKS(50));
    cli_printf("diag %s\n", argv[1]);
    return 0;
}

static void print_meter_line(int64_t t0_us)
{
    app_snapshot_t s;
    app_get_snapshot(&s);
    const meter_stats_t *m = &s.meter;
    cli_printf("%lld,%d,%d,%d,%d,%d,%lu,%d,%d\n", (esp_timer_get_time() - t0_us) / 1000,
               m->avg_rpm, m->inst_rpm, m->min_rpm, m->max_rpm, m->revs,
               (unsigned long)m->edges, m->rejected_max_rpm, m->rejected_relative);
}

static int cmd_meter(int argc, char **argv)
{
    if (argc == 2 && (!strcmp(argv[1], "on") || !strcmp(argv[1], "off"))) {
        bool on = !strcmp(argv[1], "on");
        app_set_mode(on ? APP_MODE_METER : APP_MODE_RUN);
        vTaskDelay(pdMS_TO_TICKS(50));
        cli_printf("meter %s%s\n", argv[1], on ? " (RPM channel on, display shows live RPM)" : "");
        return 0;
    }
    if (app_get_mode() != APP_MODE_METER) {
        cli_printf("Run 'meter on' first.\n");
        return 1;
    }

    cli_printf("t_ms,avg_rpm,inst_rpm,min_rpm,max_rpm,revs,edges,rej_max_rpm,rej_relative\n");
    int64_t t0 = esp_timer_get_time();
    if (argc == 1) {
        print_meter_line(t0);
        return 0;
    }
    if (strcmp(argv[1], "log") || argc > 4) {
        cli_printf("usage: meter on|off | meter | meter log [seconds] [interval_ms]\n");
        return 1;
    }
    int seconds = argc >= 3 ? atoi(argv[2]) : 10;
    int interval_ms = argc >= 4 ? atoi(argv[3]) : 500;
    if (seconds < 1 || seconds > 3600 || interval_ms < 50) {
        cli_printf("seconds 1-3600, interval_ms >= 50\n");
        return 1;
    }
    for (int64_t end = t0 + seconds * 1000000LL; esp_timer_get_time() < end;) {
        print_meter_line(t0);
        if (!cli_wait_ms(interval_ms)) {
            print_stopped();
            break;
        }
    }
    return 0;
}

// "black" or "white"
static bool parse_launcher(const char *arg, launcher_t *out)
{
    for (int l = 0; l < LAUNCHER_COUNT; l++) {
        if (!strcmp(arg, launcher_name(l))) {
            *out = l;
            return true;
        }
    }
    return false;
}

// Saves the comparator threshold of one launcher, and uses it if BX Tacho is on that one
static void set_rpm_threshold(launcher_t launcher, int mv)
{
    mv = mv < 0 ? 0 : (mv > RPM_THR_MAX_MV ? RPM_THR_MAX_MV : mv);
    settings_cal(launcher)->rpm_thr_mv = (uint16_t)mv;
    settings_save();
    if (launcher == load_launcher()) {
        rpm_set_threshold_mv(mv);
    }
    cli_printf("%s launcher: threshold set to %d mV and saved\n", launcher_name(launcher), mv);
}

// Where `rpm scan apply` puts the threshold between the black (0) and white
// (100) levels: above the middle, nearer the white level
#define RPM_SCAN_THR_PCT 60

// `apply` saves the RPM_SCAN_THR_PCT point as `launcher`'s threshold
static int rpm_scan(int seconds, bool apply, launcher_t launcher)
{
    bool was_on = rpm_is_powered();
    rpm_power(true);
    // The load LED's light off a top or the pin reaches the RPM sensor and would
    // raise the white level found: keep it off (no diag measurements) meanwhile
    load_led_hold_t hold = load_led_held();
    load_led_hold(LOAD_LED_OFF);

    cli_printf("Sampling RPM_SENSE for %d s (load LED off). Turn the launcher slowly so the sensor sees black "
               "and white...\n", seconds);
    int lo = INT_MAX;
    int hi = INT_MIN;
    bool stopped = false;
    for (int i = 0; i < seconds * 1000 && !stopped; i++) {
        int mv = rpm_read_sense_mv(1);
        if (mv >= 0) {
            lo = mv < lo ? mv : lo;
            hi = mv > hi ? mv : hi;
        }
        vTaskDelay(1);
        stopped = i % 50 == 49 && cli_stop_requested();
    }
    load_led_hold(hold);
    if (!was_on) {
        rpm_power(false);
    }
    if (stopped) {
        print_stopped();
        return 1;   // nothing applied
    }

    if (lo > hi) {
        cli_printf("ADC read failed\n");
        return 1;
    }
    int thr = lo + (hi - lo) * RPM_SCAN_THR_PCT / 100;
    cli_printf("min %d mV, max %d mV, contrast %d mV, %d %% point %d mV\n", lo, hi, hi - lo, RPM_SCAN_THR_PCT,
               thr);
    if (hi - lo < 200) {
        cli_printf("WARNING: contrast below 200 mV. Move the sensor closer or adjust R13/R15.\n");
    }
    if (thr > RPM_THR_MAX_MV) {
        cli_printf("That is above the comparator's %d mV limit; it will be capped.\n", RPM_THR_MAX_MV);
    }
    if (apply) {
        set_rpm_threshold(launcher, thr);
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
        cli_printf("rpm channel %s\n", argv[1]);
        return 0;
    }
    if (!strcmp(argv[1], "thr")) {
        // thr: show; thr black|white <mV>: set one launcher's (always named, so a
        // launcher not detected yet can't take the other one's value)
        launcher_t launcher;
        if (argc == 4 && parse_launcher(argv[2], &launcher)) {
            set_rpm_threshold(launcher, atoi(argv[3]));
        } else if (argc != 2) {
            cli_printf("Name the launcher: rpm thr black|white <mV>\n");
            return 1;
        } else {
            cli_printf("threshold %d mV in use (%s launcher); black %u mV, white %u mV\n", rpm_get_threshold_mv(),
                       launcher_name(load_launcher()), settings_cal(LAUNCHER_BLACK)->rpm_thr_mv,
                       settings_cal(LAUNCHER_WHITE)->rpm_thr_mv);
        }
        return 0;
    }
    if (!strcmp(argv[1], "sense")) {
        if (!rpm_is_powered()) {
            cli_printf("RPM channel is off; RPM_SENSE is only valid while powered ('rpm on' in diag mode)\n");
            return 1;
        }
        int n = argc == 3 ? atoi(argv[2]) : 10;
        for (int i = 0; i < n; i++) {
            cli_printf("RPM_SENSE %d mV\n", rpm_read_sense_mv(16));
            if (!cli_wait_ms(100)) {
                print_stopped();
                break;
            }
        }
        return 0;
    }
    if (!strcmp(argv[1], "scan")) {
        if (!require_diag()) {
            return 1;
        }
        // scan [black|white] [seconds] [apply]: apply needs the launcher named
        int seconds = 5;
        bool apply = false;
        bool named = false;
        launcher_t launcher = LAUNCHER_BLACK;
        for (int i = 2; i < argc; i++) {
            if (!strcmp(argv[i], "apply")) {
                apply = true;
            } else if (parse_launcher(argv[i], &launcher)) {
                named = true;
            } else {
                seconds = atoi(argv[i]);
            }
        }
        if (apply && !named) {
            cli_printf("Name the launcher to apply the result to: rpm scan black|white [seconds] apply\n");
            return 1;
        }
        if (seconds < 1 || seconds > 60) {
            seconds = 5;
        }
        return rpm_scan(seconds, apply, launcher);
    }
usage:
    cli_printf("usage: rpm on|off | thr [black|white <mV>] | sense [count] | scan [black|white] [seconds] [apply]\n");
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
    if (argc == 3 && !strcmp(argv[1], "led")) {
        if (!strcmp(argv[2], "auto")) {
            load_led_hold(LOAD_LED_AUTO);
            cli_printf("load IR LED back to normal: measurements light it as they need\n");
            return 0;
        }
        bool on = !strcmp(argv[2], "on");
        if (!on && strcmp(argv[2], "off")) {
            cli_printf("usage: load led on|off|auto\n");
            return 1;
        }
        // Diag, or meter mode (which never drives the load LED) to test it at speed
        app_mode_t mode = app_get_mode();
        if (mode != APP_MODE_DIAG && mode != APP_MODE_METER) {
            cli_printf("Run 'diag on' or 'meter on' first.\n");
            return 1;
        }
        load_led_hold(on ? LOAD_LED_ON : LOAD_LED_OFF);
        cli_printf("load IR LED held %s%s; readings show the raw LOAD_SENSE until 'load led auto'\n",
                   argv[2], on ? " (as while a top is loaded)" : " (only ambient light)");
        return 0;
    }
    load_led_hold_t hold = load_led_held();
    if (argc >= 2 && !strcmp(argv[1], "flash")) {
        // A full poll next to the short flash used while the RPM channel is armed,
        // to check that the flash settles to the same level; optionally with
        // another settling time (us)
        int n = argc >= 3 ? atoi(argv[2]) : 10;
        int settle_us = argc >= 4 ? atoi(argv[3]) : 0;
        if (hold != LOAD_LED_AUTO) {
            cli_printf("The load LED is held; run 'load led auto' first.\n");
            return 1;
        }
        if (settle_us < 0 || settle_us > 5000) {
            cli_printf("settle 0-5000 us (0: the firmware's own)\n");
            return 1;
        }
        for (int i = 0; i < n; i++) {
            int poll_on, poll_off, flash_on, flash_off;
            int poll = load_measure_raw(4, &poll_on, &poll_off);
            vTaskDelay(pdMS_TO_TICKS(20));
            int flash = settle_us ? load_flash_sample_settle_raw(settle_us, &flash_on, &flash_off)
                                  : load_flash_sample_raw(&flash_on, &flash_off);
            cli_printf("poll %d mV (ON %d OFF %d), flash %d mV (ON %d OFF %d)\n", poll, poll_on, poll_off, flash,
                       flash_on, flash_off);
            if (!cli_wait_ms(200)) {
                print_stopped();
                break;
            }
        }
        return 0;
    }
    int n = argc == 2 ? atoi(argv[1]) : 10;
    for (int i = 0; i < n; i++) {
        if (hold != LOAD_LED_AUTO) {
            cli_printf("LOAD_SENSE (raw, LED held %s) %d mV\n", hold == LOAD_LED_ON ? "on" : "off",
                       load_read_raw_mv(8));
        } else {
            cli_printf("LOAD_SENSE (LED on - off) %d mV\n", load_measure(8));
        }
        if (!cli_wait_ms(200)) {
            print_stopped();
            break;
        }
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
    cli_printf("VBAT calibration. Setup:\n"
               "  - Power the board ONLY from the supply on J1 (+ to pad 1 VBAT+, - to pad 2 GND),\n"
               "    power switch ON, current limit about 300 mA. No battery connected.\n"
               "  - Run the console over Bluetooth (web app), or over USB data-only (VBUS cut):\n"
           "    with USB power the charger drives VBAT.\n"
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
        cli_printf("USB power (VBUS) detected: the charger would drive VBAT and spoil the reading.\n"
                   "Unplug USB and use the console over Bluetooth (or a data-only USB connection),\n"
                   "with the board powered from the supply on J1.\n");
        return false;
    }
    return true;
}

// Average of 8 groups of 16 readings at the VBAT_SENSE pin; *spread = max - min
// of the groups. The display stays frozen meanwhile (about 0.5 s): redraws
// change the board's current, and so the supply voltage, a little.
static int measure_vbat_pin(int *spread)
{
    app_hold_display(true);
    vTaskDelay(pdMS_TO_TICKS(VBAT_POINT_SETTLE_MS));
    int lo = INT_MAX;
    int hi = INT_MIN;
    int sum = 0;
    for (int g = 0; g < 8; g++) {
        int mv = battery_read_pin_mv(16);
        if (mv < 0) {
            app_hold_display(false);
            return -1;
        }
        sum += mv;
        lo = mv < lo ? mv : lo;
        hi = mv > hi ? mv : hi;
        vTaskDelay(pdMS_TO_TICKS(25));
    }
    app_hold_display(false);
    *spread = hi - lo;
    return (sum + 4) / 8;
}

static void print_vbat_cal(const vbat_cal_t *c)
{
    cli_printf("gain %ld.%06ld, offset %ld mV, %u points over %d-%d mV, max fit error %d mV\n",
               (long)(c->gain_ppm / 1000000), (long)(c->gain_ppm % 1000000), (long)c->offset_mv,
               c->points, c->min_mv, c->max_mv, c->max_err_mv);
}

static int cmd_cal_vbat(int argc, char **argv)
{
    const char *sub = argc >= 2 ? argv[1] : "";

    if (!strcmp(sub, "show") && argc == 2) {
        if (vbat_cal_valid()) {
            cli_printf("stored VBAT calibration: ");
            print_vbat_cal(vbat_cal_get());
        } else {
            cli_printf("no VBAT calibration stored (readings use the nominal divider ratio x2)\n");
        }
        for (int i = 0; i < vbat_cal_session_points(); i++) {
            int pin, vbat;
            vbat_cal_session_point(i, &pin, &vbat);
            cli_printf("  session point %d: VBAT %d mV, pin %d mV\n", i + 1, vbat, pin);
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
            cli_printf("Not in VBAT calibration mode.\n");
            return 1;
        }
        app_set_mode(APP_MODE_RUN);
        vTaskDelay(pdMS_TO_TICKS(50));
        cli_printf("VBAT calibration mode ended\n");
        return 0;
    }

    if (app_get_mode() != APP_MODE_VBATCAL) {
        cli_printf("Run 'cal vbat start' first.\n");
        return 1;
    }
    if (!vbat_cal_usb_power_check()) {
        return 1;
    }

    if (!strcmp(sub, "point") && argc == 3) {
        int vbat = atoi(argv[2]);
        if (vbat < VBAT_CAL_MIN_MV || vbat > VBAT_CAL_MAX_MV) {
            cli_printf("Enter the measured voltage in mV, %d-%d.\n", VBAT_CAL_MIN_MV, VBAT_CAL_MAX_MV);
            return 1;
        }
        for (int i = 0; i < vbat_cal_session_points(); i++) {
            int pin, have;
            vbat_cal_session_point(i, &pin, &have);
            if (abs(have - vbat) < VBAT_POINT_MIN_GAP) {
                cli_printf("Already have a point at %d mV; use points at least %d mV apart "
                           "('cal vbat start' starts over).\n", have, VBAT_POINT_MIN_GAP);
                return 1;
            }
        }
        int spread;
        int pin = measure_vbat_pin(&spread);
        if (pin < 0) {
            cli_printf("ADC read failed\n");
            return 1;
        }
        if (spread > VBAT_POINT_MAX_SPREAD) {
            cli_printf("Reading unstable (%d mV spread at the pin). Check the supply and connections.\n", spread);
            return 1;
        }
        int nominal = vbat_cal_nominal(pin);
        if (abs(nominal - vbat) > VBAT_POINT_PLAUSIBLE) {
            cli_printf("Measured %d mV nominal vs %d mV entered: too far apart. Check the value and wiring.\n",
                       nominal, vbat);
            return 1;
        }
        if (vbat_cal_session_add(pin, vbat) != ESP_OK) {
            cli_printf("Maximum of %d points reached. 'cal vbat save' or 'cal vbat start' to start over.\n",
                       VBAT_CAL_MAX_POINTS);
            return 1;
        }
        cli_printf("point %d: VBAT %d mV, pin %d mV (nominal reading %d mV, %+d mV)\n",
                   vbat_cal_session_points(), vbat, pin, nominal, nominal - vbat);
        return 0;
    }
    if (!strcmp(sub, "save") && argc == 2) {
        vbat_cal_t cal = {0};
        int resid[VBAT_CAL_MAX_POINTS] = {0};
        const char *why = vbat_cal_session_fit(&cal, resid);
        // cal.points is set whenever a line was fitted, also if it was then rejected
        if (cal.points > 0) {
            cli_printf("  VBAT mV   pin mV   fit error\n");
            for (int i = 0; i < vbat_cal_session_points(); i++) {
                int pin, vbat;
                vbat_cal_session_point(i, &pin, &vbat);
                cli_printf("  %7d  %7d  %+6d mV\n", vbat, pin, resid[i]);
            }
        }
        if (why) {
            cli_printf("Not saved: %s.\n", why);
            return 1;
        }
        if (vbat_cal_save(&cal) != ESP_OK) {
            cli_printf("Saving failed (factory partition unavailable?).\n");
            return 1;
        }
        cli_printf("Saved: ");
        print_vbat_cal(&cal);
        cli_printf("Now check it: set another voltage and run 'cal vbat verify <mV>'.\n");
        return 0;
    }
    if (!strcmp(sub, "verify") && argc == 3) {
        if (!vbat_cal_valid()) {
            cli_printf("No calibration stored yet ('cal vbat save').\n");
            return 1;
        }
        int actual = atoi(argv[2]);
        if (actual < VBAT_CAL_MIN_MV || actual > VBAT_CAL_MAX_MV) {
            cli_printf("Enter the measured voltage in mV, %d-%d.\n", VBAT_CAL_MIN_MV, VBAT_CAL_MAX_MV);
            return 1;
        }
        int spread;
        int pin = measure_vbat_pin(&spread);
        if (pin < 0) {
            cli_printf("ADC read failed\n");
            return 1;
        }
        int measured = vbat_cal_apply(pin);
        int err = measured - actual;
        bool pass = abs(err) <= VBAT_CAL_VERIFY_TOL_MV && spread <= VBAT_POINT_MAX_SPREAD;
        cli_printf("VBAT %d mV measured vs %d mV actual: %+d mV (nominal would be %+d mV)%s -> %s\n",
                   measured, actual, err, vbat_cal_nominal(pin) - actual,
                   spread > VBAT_POINT_MAX_SPREAD ? ", reading unstable" : "", pass ? "PASS" : "FAIL");
        return pass ? 0 : 1;
    }

    cli_printf("usage: cal vbat start | point <mV> | save | verify <mV> | show | stop\n");
    return 1;
}

static int cmd_cal_load(int argc, char **argv)
{
    // cal load black|white empty|loaded|clear: always named, so a launcher not
    // detected yet can't take the other one's calibration
    launcher_t launcher;
    if (argc != 3 || !parse_launcher(argv[1], &launcher)) {
        return -1;
    }
    const char *what = argv[2];

    if (!strcmp(what, "clear")) {
        settings_cal(launcher)->load_cal_valid = false;
        s_have_empty[launcher] = s_have_loaded[launcher] = false;
        settings_save();
        cli_printf("%s launcher: load calibration cleared\n", launcher_name(launcher));
        return 0;
    }
    bool empty = !strcmp(what, "empty");
    if (!empty && strcmp(what, "loaded")) {
        return -1;
    }
    if (load_led_held() != LOAD_LED_AUTO) {
        cli_printf("The load LED is held; run 'load led auto' first.\n");
        return 1;
    }
    int level = average_load();
    if (level == INT_MIN) {
        cli_printf("ADC read failed\n");
        return 1;
    }
    launcher_cal_t *cal = settings_cal(launcher);
    if (empty) {
        cal->load_empty = (int16_t)level;
        s_have_empty[launcher] = true;
    } else {
        cal->load_loaded = (int16_t)level;
        s_have_loaded[launcher] = true;
    }
    cli_printf("%s launcher: %s level %d mV\n", launcher_name(launcher), what, level);

    if ((s_have_empty[launcher] && s_have_loaded[launcher]) || cal->load_cal_valid) {
        int span = abs(cal->load_loaded - cal->load_empty);
        if (span < LOAD_MIN_CONTRAST_MV) {
            cli_printf("Contrast only %d mV (need %d). Not saved as valid.\n", span, LOAD_MIN_CONTRAST_MV);
            cal->load_cal_valid = false;
        } else {
            cal->load_cal_valid = true;
            cli_printf("Calibration valid, contrast %d mV.\n", span);
        }
    } else {
        cli_printf("Now run 'cal load %s %s'.\n", launcher_name(launcher), empty ? "loaded" : "empty");
    }
    settings_save();
    return 0;
}

static int cmd_cal(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], "vbat")) {
        return cmd_cal_vbat(argc - 1, argv + 1);
    }
    if (argc >= 2 && !strcmp(argv[1], "load")) {
        int ret = cmd_cal_load(argc - 1, argv + 1);
        if (ret >= 0) {
            return ret;
        }
        goto usage;
    }
usage:
    cli_printf("usage: cal load black|white empty|loaded|clear\n"
               "       cal vbat start | point <mV> | save | verify <mV> | show | stop\n");
    return 1;
}

static int cmd_cfg(int argc, char **argv)
{
    settings_t *cfg = settings_get();

    if (argc == 3 && !strcmp(argv[1], "edges")) {
        int n = atoi(argv[2]);
        if (n < 1 || n > 16) {
            cli_printf("edges must be 1-16\n");
            return 1;
        }
        cfg->edges_per_rev = (uint8_t)n;
        settings_save();
        cli_printf("edges per revolution: %d\n", n);
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "reset")) {
        settings_reset();
        settings_save();
        rpm_set_threshold_mv(settings_cal(load_launcher())->rpm_thr_mv);
        memset(s_have_empty, 0, sizeof(s_have_empty));
        memset(s_have_loaded, 0, sizeof(s_have_loaded));
        cli_printf("settings reset to defaults\n");
        return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "flip") && (!strcmp(argv[2], "on") || !strcmp(argv[2], "off"))) {
        bool on = !strcmp(argv[2], "on");
        settings_set_flip(on);
        display_set_flipped(on);
        cli_printf("display %s (saved)\n", on ? "turned 180 degrees" : "the normal way up");
        return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "ext") && (!strcmp(argv[2], "on") || !strcmp(argv[2], "off"))) {
        const char *ext = bbp_ext_built_in();
        if (ext == NULL) {
            cli_printf("This build has no Battle Pass extension.\n");
            return 1;
        }
        bool on = !strcmp(argv[2], "on");
        if (on == settings_bbp_ext()) {
            cli_printf("extension %s is already %s\n", ext, argv[2]);
            return 0;
        }
        settings_set_bbp_ext(on);
        cli_printf("extension %s %s (saved). Restarting to apply...\n", ext, argv[2]);
        vTaskDelay(pdMS_TO_TICKS(300));
        esp_restart();
    }
    if (argc == 3 && !strcmp(argv[1], "lightsleep") && (!strcmp(argv[2], "on") || !strcmp(argv[2], "off"))) {
        bool on = !strcmp(argv[2], "on");
        settings_set_light_sleep(on);
        app_set_light_sleep(on);
        cli_printf("light sleep on battery: %s (saved)\n", argv[2]);
        return 0;
    }
    cli_printf("usage: cfg edges <1-16> | cfg lightsleep on|off | cfg flip on|off | cfg ext on|off | cfg reset\n");
    return 1;
}

// Microseconds as milliseconds with one decimal, right-aligned in 7 characters
static void print_ms(uint32_t us)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "%lu.%lu", (unsigned long)(us / 1000), (unsigned long)(us % 1000 / 100));
    cli_printf("%7s", buf);
}

// Last launch, edge by edge: interval, RPM over one revolution, the release
// seen by the load sensor and the edges the chatter filter dropped
static int cmd_launch(int argc, char **argv)
{
    static uint32_t edges[200];
    static launch_trace_t info;
    int max = argc > 1 ? atoi(argv[1]) : 60;
    if (argc > 2 || max < 2 || max > (int)(sizeof(edges) / sizeof(edges[0]))) {
        cli_printf("usage: launch [edges 2-200, default 60]\n");
        return 1;
    }
    if (!launch_get_trace(edges, max, &info)) {
        cli_printf("No finished launch yet.\n");
        return 1;
    }
    int epr = settings_get()->edges_per_rev;
    int n = info.num_edges < max ? info.num_edges : max;
    uint32_t t0 = edges[0];

    cli_printf("%d edges (showing %d), %d edge(s)/rev, release %s", info.num_edges, n, epr,
               info.release_seen ? "at " : "not seen by the load sensor");
    if (info.release_seen) {
        uint32_t rel = info.release_us - t0;
        cli_printf("+%lu.%lu ms", (unsigned long)(rel / 1000), (unsigned long)(rel % 1000 / 100));
    }
    cli_printf(", %d edge(s) dropped by the filter", info.num_rejected);
    if (info.lock_recoveries > 0) {
        cli_printf(", filter lock recovered %d time(s)", info.lock_recoveries);
    }
    cli_printf("\n");
    cli_printf("  #   t (ms)  interval (us)  rpm (1 rev)\n");

    int r = 0;   // next dropped edge to print
    bool release_printed = !info.release_seen;
    for (int i = 0; i < n; i++) {
        // Dropped edges and the release, in time order before this edge
        while (r < info.num_rejected && (int32_t)(info.rejected_us[r] - edges[i]) < 0) {
            cli_printf("     ");
            print_ms(info.rejected_us[r] - t0);
            cli_printf("   -- dropped (%s)\n", info.rejected_max_rpm[r] ? "max rpm" : "too soon");
            r++;
        }
        if (!release_printed && (int32_t)(info.release_us - edges[i]) < 0) {
            cli_printf("     ");
            print_ms(info.release_us - t0);
            cli_printf("   -- release (load sensor)\n");
            release_printed = true;
        }
        cli_printf("%3d  ", i);
        print_ms(edges[i] - t0);
        if (i > 0) {
            cli_printf("  %13lu", (unsigned long)(edges[i] - edges[i - 1]));
        }
        if (i >= epr) {
            cli_printf("  %11lu", (unsigned long)(60000000ULL / (edges[i] - edges[i - epr])));
        }
        cli_printf("\n");
    }
    return 0;
}

static int cmd_ble(int argc, char **argv)
{
    if (argc == 2 && (!strcmp(argv[1], "on") || !strcmp(argv[1], "off"))) {
        bool on = !strcmp(argv[1], "on");
        if (on == settings_ble()) {
            cli_printf("BLE is already %s\n", argv[1]);
            return 0;
        }
        settings_set_ble(on);
        cli_printf("BLE %s (saved). Restarting to apply...\n", argv[1]);
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_restart();
    }
    if (argc != 1) {
        cli_printf("usage: ble | ble on|off\n");
        return 1;
    }

    bbp_info_t b;
    bbp_get_info(&b);
    cli_printf("BLE: %s", b.radio_on ? "on, " BBP_DEVICE_NAME : "off");
    if (b.radio_on) {
        cli_printf(", %s", b.subscribed ? "app listening" : (b.connected ? "connected" : "advertising"));
    }
    cli_printf("\n");
    const char *ext = bbp_ext_built_in();
    if (ext != NULL) {
        cli_printf("extension: %s, %s%s ('cfg ext on|off')\n", ext, settings_bbp_ext() ? "on" : "off",
                   settings_bbp_ext() && b.radio_on && !bbp_ext_active() ? " but not in use (wrong version)" : "");
    }
    return 0;
}

// The Battle Pass shot history that apps read (kept with BLE off too)
static int cmd_history(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "clear")) {
        bbp_clear_history();
        cli_printf("Shot history cleared: last %d shots, total count, max and latest curve\n",
                   BBP_HISTORY_LEN);
        return 0;
    }
    if (argc != 1) {
        cli_printf("usage: history | history clear\n");
        return 1;
    }

    bbp_info_t b;
    bbp_get_info(&b);
    cli_printf("shots   : %d in history, %lu total, max %u\n", b.count, (unsigned long)b.total, b.max_sp);
    for (int i = 0; i < b.count; i++) {
        // Ten per line, oldest first
        cli_printf("%s%5u", i % 10 ? " " : (i ? "\n          " : "history : "), b.sp[i]);
    }
    if (b.count > 0) {
        cli_printf("\n");
    }
    return 0;
}

static int cmd_sleep(int argc, char **argv)
{
    cli_printf("Deep sleep in %d s. Unplug USB now, otherwise sleep is cancelled.\n"
               "Wake up later by plugging in USB or cycling the power switch.\n", APP_SLEEP_COUNTDOWN_S);
    if (!app_enter_deep_sleep(APP_SLEEP_COMMAND)) {
        cli_printf("USB still connected: deep sleep cancelled.\n");
        return 1;
    }
    return 0;
}

// Chip identity from eFuse. The factory MAC is unique per chip (it's what
// Espressif uses as the chip ID), so it serves as the serial number.
static int cmd_id(int argc, char **argv)
{
    uint8_t mac[6];
    uint8_t bt[6];
    if (esp_efuse_mac_get_default(mac) != ESP_OK || esp_read_mac(bt, ESP_MAC_BT) != ESP_OK) {
        cli_printf("Reading the MAC from eFuse failed.\n");
        return 1;
    }
    cli_printf("serial      : %02X%02X%02X%02X%02X%02X (factory MAC, unique per chip)\n",
               mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    cli_printf("bluetooth   : %02X:%02X:%02X:%02X:%02X:%02X\n", bt[0], bt[1], bt[2], bt[3], bt[4], bt[5]);

    // Written at the factory on some chips only; blank (all zero) on others
    uint8_t uid[16] = {0};
    bool have_uid = false;
    if (esp_efuse_read_field_blob(ESP_EFUSE_OPTIONAL_UNIQUE_ID, uid, sizeof(uid) * 8) == ESP_OK) {
        for (int i = 0; i < sizeof(uid); i++) {
            have_uid |= uid[i] != 0;
        }
    }
    cli_printf("unique id   : ");
    if (have_uid) {
        for (int i = 0; i < sizeof(uid); i++) {
            cli_printf("%02X", uid[i]);
        }
        cli_printf(" (eFuse, 128 bits)\n");
    } else {
        cli_printf("not programmed on this chip\n");
    }

    esp_chip_info_t chip;
    esp_chip_info(&chip);
    cli_printf("chip        : ESP32-C3 revision v%d.%d\n", chip.revision / 100, chip.revision % 100);
    return 0;
}

static int cmd_events(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "clear")) {
        eventlog_clear();
        cli_printf("event log cleared\n");
        return 0;
    }
    if (argc != 1) {
        cli_printf("usage: events | events clear\n");
        return 1;
    }
    eventlog_print();
    return 0;
}

static int cmd_help(int argc, char **argv);

typedef struct {
    const char *name;
    const char *help;
    esp_console_cmd_func_t func;
} cli_cmd_t;

static const cli_cmd_t s_cmds[] = {
    {"help", "help [command]: list the commands, or explain one", cmd_help},
    {"status", "Show sensors, battery, calibration and the last launch", cmd_status},
    {"diag", "diag on|off: pause the state machine for manual sensor control", cmd_diag},
    {"meter", "meter on|off | meter | meter log [seconds] [interval_ms]: continuous RPM (CSV); Ctrl-C stops the log", cmd_meter},
    {"rpm", "rpm on|off | thr [black|white <mV>] | sense [count] | scan [black|white] [seconds] [apply] (thresholds per launcher: setting one or applying a scan needs the launcher named)", cmd_rpm},
    {"load", "load [count]: print ambient-cancelled LOAD_SENSE readings; load flash [count] [settle_us]: a full poll next to a short flash sample (as while a top is loaded); load led on|off|auto: hold the load LED on or off (diag or meter mode; readings then show the raw LOAD_SENSE), or back to normal", cmd_load},
    {"cal", "cal load black|white empty|loaded|clear: load sensor, per launcher; cal vbat start|point <mV>|save|verify <mV>|show|stop: production VBAT calibration (kept by 'cfg reset')", cmd_cal},
    {"cfg", "cfg edges <n> | cfg lightsleep on|off | cfg flip on|off (display turned 180 degrees, for the other hand) | cfg ext on|off (optional Battle Pass extension, in builds that have one; restarts) | cfg reset", cmd_cfg},
    {"launch", "launch [n]: last launch edge by edge (first n edges, default 60), with the release and dropped edges", cmd_launch},
    {"ble", "ble | ble on|off: Battle Pass emulation and the console over BLE (on|off restarts)", cmd_ble},
    {"history", "history | history clear: shot history sent to Battle Pass apps (last 50, total, max, latest curve)", cmd_history},
    {"sleep", "Enter deep sleep now", cmd_sleep},
    {"events", "events | events clear: power/sleep event log (survives deep sleep and resets)", cmd_events},
    {"id", "Serial number (factory MAC), Bluetooth address, eFuse unique ID and chip revision", cmd_id},
};
#define NUM_CMDS (sizeof(s_cmds) / sizeof(s_cmds[0]))

// Same output on every console (esp_console's help prints to stdout)
static int cmd_help(int argc, char **argv)
{
    bool found = false;
    for (int i = 0; i < NUM_CMDS; i++) {
        if (argc < 2 || !strcmp(argv[1], s_cmds[i].name)) {
            cli_printf("%s\n  %s\n\n", s_cmds[i].name, s_cmds[i].help);
            found = true;
        }
    }
    if (!found) {
        cli_printf("No command '%s'. Type 'help' for the list.\n", argv[1]);
        return 1;
    }
    return 0;
}

// Every command runs through here, from any console
static int run_cmd(void *context, int argc, char **argv)
{
    const cli_cmd_t *cmd = context;
    if (xSemaphoreTake(s_cmd_lock, 0) != pdTRUE) {
        cli_printf("Busy: the other console (USB or Bluetooth) is running a command. "
                   "Try again when it ends.\n");
        return 1;
    }
    if (t_console == NULL) {
        s_usb_stop = false;
    }
    app_note_activity();
    int ret = cmd->func(argc, argv);
    xSemaphoreGive(s_cmd_lock);
    return ret;
}

void cli_bind_console(cli_console_t *console)
{
    t_console = console;
}

void cli_run_line(char *line)
{
    char *argv[CLI_MAX_ARGS];
    if (t_console != NULL) {
        t_console->stop = false;
    }
    size_t argc = esp_console_split_argv(line, argv, CLI_MAX_ARGS);
    if (s_cmd_lock == NULL) {
        cli_printf("The console is still starting. Try again in a moment.\n");
    } else if (argc > 0) {
        const cli_cmd_t *cmd = NULL;
        for (int i = 0; i < NUM_CMDS && cmd == NULL; i++) {
            cmd = !strcmp(argv[0], s_cmds[i].name) ? &s_cmds[i] : NULL;
        }
        if (cmd != NULL) {
            run_cmd((void *)cmd, argc, argv);
        } else {
            cli_printf("Unknown command '%s'. Type 'help' for the list.\n", argv[0]);
        }
    }
}

esp_err_t cli_start(void)
{
    s_cmd_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_cmd_lock != NULL, ESP_ERR_NO_MEM, "cli", "lock");

    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_cfg.prompt = "bxt>";
    ESP_ERROR_CHECK(esp_console_new_repl_stdio(&repl_cfg, &repl));
    // The console asks the terminal once, at boot, whether it handles escape
    // sequences, and falls back to plain line mode for good if nothing answers.
    // Over USB nothing is attached that early (the web app, idf.py monitor and
    // PuTTY connect later and all handle them), so keep line editing and history.
    linenoiseSetDumbMode(0);

    for (int i = 0; i < NUM_CMDS; i++) {
        const esp_console_cmd_t cmd = {
            .command = s_cmds[i].name,
            .help = s_cmds[i].help,
            .func_w_context = run_cmd,
            .context = (void *)&s_cmds[i],
        };
        ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
    }
    return esp_console_start_repl(repl);
}
