// Bring-up and calibration console: the REPL on USB Serial/JTAG, and the same
// commands for other consoles (BLE, ble_console.c), each from a task of its own
// bound to it with cli_bind_console().
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

esp_err_t cli_start(void);

// Where a console other than the USB REPL sends a command's output
typedef struct {
    void (*write)(const char *text, size_t len);
    volatile bool stop;    // set (from any task) to stop the running command
} cli_console_t;

// Sends everything the calling task prints with cli_printf() to `console`.
// Each console's task calls it once, before printing anything.
void cli_bind_console(cli_console_t *console);

// Runs one command line (modified in place), from a task bound to a console.
// One command runs at a time over all consoles: while another one is running
// this prints a note and returns without running it.
void cli_run_line(char *line);

// Commands print with this rather than printf, so the output goes to the
// console that runs them (picolibc's stdout is shared by all tasks)
int cli_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// For long-running commands: true once the user asked to stop (Ctrl-C).
// cli_wait_ms() waits that long, or less if stopped, and returns !stopped.
bool cli_stop_requested(void);
bool cli_wait_ms(int ms);
