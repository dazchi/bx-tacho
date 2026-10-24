// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Darren Chi
// Optional extension of the Battle Pass emulation (main/bbp.c). An independent
// component named bbp_ext, kept outside this repository, can provide these
// functions. It is built in when one of EXTRA_COMPONENT_DIRS has it (firmware
// README "Optional extension") and used while `cfg ext on` (off by default,
// applied at boot). Without it none of this is called and BX Tacho behaves as
// it does without an extension.
//
// Linking an extension that uses only this interface is covered by the
// additional permission in NOTICE (GPL version 3, section 7).
//
// Everything is called from BX Tacho's own tasks, never from an interrupt, and
// never at the same time from two tasks. A "session" number changes with every
// connect, disconnect and subscription change of the Battle Pass link.
#pragma once

#include <stdbool.h>
#include <stdint.h>

// Bumped whenever these declarations change; BX Tacho doesn't use an extension
// built for another version
#define BBP_EXT_API_VERSION 1

#define BBP_EXT_PKT_LEN 17

// The BBP_EXT_API_VERSION the extension was built with
int bbp_ext_api_version(void);
// Short name, for the console and the log
const char *bbp_ext_name(void);

// Advertising: true puts the service UUID in the advertisement and the name in
// the scan response; false is the default (name in the advertisement)
bool bbp_ext_uuid_in_advertisement(void);
// GATT flags of the data characteristic (f002), given the default ones; called
// once, before BLE starts
uint16_t bbp_ext_data_flags(uint16_t flags);

// A status packet (A0) about to be sent in `session`: may change it
void bbp_ext_status(uint8_t pkt[BBP_EXT_PKT_LEN], uint32_t session);
// The app subscribed to notifications at `subscribed_us` (esp_timer time);
// called right after the initial status of the session has gone out
void bbp_ext_session_start(uint32_t session, int64_t subscribed_us);

typedef struct {
    int next_ms;            // call bbp_ext_poll again within this many ms; < 0: no need
    bool send_status;       // send a status packet now (it goes through bbp_ext_status)
    const char *reason;     // why, for the log
} bbp_ext_action_t;

// Called after every event the emulation handles, and once next_ms has passed
bbp_ext_action_t bbp_ext_poll(uint32_t session, bool subscribed, bool attached, int64_t now_us);
