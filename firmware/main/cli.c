#include "cli.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app.h"
#include "esp_console.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "load.h"
#include "rpm.h"
#include "settings.h"

static bool s_have_empty;
static bool s_have_loaded;

static bool require_diag(void)
{
    if (!app_is_diag()) {
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

    printf("mode        : %s\n", s.diag ? "diag" : "run");
    printf("state       : %s\n", launch_state_name(s.state));
    printf("power       : %s, VBAT %d mV (%s), %d%%\n", src_name(s.batt.src), s.batt.vbat_mv,
           s.batt.vbat_valid ? "valid" : "not trusted while on USB", s.batt.percent);
    printf("rpm channel : %s, threshold %d mV, %u edges/rev, %lu edges dropped\n",
           rpm_is_powered() ? "on" : "off", rpm_get_threshold_mv(), cfg->edges_per_rev,
           (unsigned long)rpm_overflow_count());
    if (cfg->load_cal_valid) {
        printf("load        : %s, %d mV (%d%%), empty %d mV, loaded %d mV\n",
               s.loaded ? "LOADED" : "empty", s.load_diff_mv, s.load_pct,
               cfg->load_empty, cfg->load_loaded);
    } else {
        printf("load        : not calibrated (RPM stays armed). See 'cal load'.\n");
    }
    if (s.have_result) {
        printf("last launch : peak %d rpm, max %d rpm, %d revs, %d ms\n", s.result.peak_rpm,
               s.result.max_rpm, s.result.revolutions, s.result.duration_ms);
    }
    printf("idle        : %lu ms\n", (unsigned long)s.idle_ms);
    return 0;
}

static int cmd_diag(int argc, char **argv)
{
    if (argc != 2 || (strcmp(argv[1], "on") && strcmp(argv[1], "off"))) {
        printf("usage: diag on|off\n");
        return 1;
    }
    app_set_diag(strcmp(argv[1], "on") == 0);
    // Let the sensor task release the hardware before returning
    vTaskDelay(pdMS_TO_TICKS(50));
    printf("diag %s\n", argv[1]);
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
    int n = argc == 2 ? atoi(argv[1]) : 10;
    for (int i = 0; i < n; i++) {
        printf("LOAD_SENSE (LED on - off) %d mV\n", load_measure(8));
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    return 0;
}

static int cmd_cal(int argc, char **argv)
{
    settings_t *cfg = settings_get();

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
    printf("usage: cal load empty|loaded|clear\n");
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
    printf("usage: cfg edges <1-16> | cfg reset\n");
    return 1;
}

static int cmd_sleep(int argc, char **argv)
{
    printf("Deep sleep. Plug in USB or cycle the power switch to wake.\n");
    vTaskDelay(pdMS_TO_TICKS(100));
    app_enter_deep_sleep();
    return 0;
}

esp_err_t cli_start(void)
{
    const esp_console_cmd_t cmds[] = {
        {.command = "status", .help = "Show sensors, battery, calibration and the last launch", .func = cmd_status},
        {.command = "diag", .help = "diag on|off: pause the state machine for manual sensor control", .func = cmd_diag},
        {.command = "rpm", .help = "rpm on|off | thr [mV] | sense [count] | scan [seconds] [apply]", .func = cmd_rpm},
        {.command = "load", .help = "load [count]: print ambient-cancelled LOAD_SENSE readings", .func = cmd_load},
        {.command = "cal", .help = "cal load empty|loaded|clear: calibrate the load sensor", .func = cmd_cal},
        {.command = "cfg", .help = "cfg edges <n> | cfg reset", .func = cmd_cfg},
        {.command = "sleep", .help = "Enter deep sleep now", .func = cmd_sleep},
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
