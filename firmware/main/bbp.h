// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Darren Chi
// BEYBATTLE PASS (BX-09) emulation over BLE, so that third-party Battle Pass
// apps can read launches. Protocol and sources: docs/bbp-protocol.md.
//
// The shot history (last 50 shoot powers, lifetime count and max, curve of the
// latest shot) is kept in NVS whether or not BLE is on.
//
// An optional extension (components/bbp_ext_api/include/bbp_ext.h) can change
// how BX Tacho presents itself, in builds that have one and while `cfg ext on`.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define BBP_DEVICE_NAME "BEYBLADE_TOOL01"
#define BBP_HISTORY_LEN 50

typedef struct {
    bool radio_on;        // BLE started (settings_ble() at boot)
    bool connected;
    bool subscribed;      // app listening to notifications
    int count;            // shots in the history (up to BBP_HISTORY_LEN)
    uint32_t total;       // shots ever recorded
    uint16_t max_sp;      // highest shoot power ever recorded
    uint16_t sp[BBP_HISTORY_LEN];   // oldest first
} bbp_info_t;

// Loads the history and starts the worker task; starts BLE if `radio`.
// Call after settings_init() (NVS).
esp_err_t bbp_init(bool radio);

// Called from the sensor task. They only queue work and never block.
void bbp_set_attached(bool attached);
void bbp_set_battery_percent(int percent);
// Shoot power (RPM) and the launch curve as revolution periods in us
void bbp_record_shot(int sp_rpm, const uint32_t *periods_us, int n);

void bbp_get_info(bbp_info_t *out);

// Optional extension (bbp_ext.h): its name if this build has one, else NULL;
// and whether it's in use (built in, `cfg ext on`, and the right version)
const char *bbp_ext_built_in(void);
bool bbp_ext_active(void);

typedef enum {
    BBP_LINK_OFF,          // BLE not started
    BBP_LINK_ADVERTISING,  // waiting for an app
    BBP_LINK_CONNECTED,    // an app is connected
} bbp_link_t;

// Cheap (no lock), for the display
bbp_link_t bbp_link_state(void);

void bbp_clear_history(void);

// Before deep sleep: stop advertising and close the connection, so the app
// sees the disconnect at once instead of after a supervision timeout
void bbp_stop(void);
