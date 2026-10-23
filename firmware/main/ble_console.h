// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Darren Chi
// The console commands over BLE, for the web app. A GATT service of its own
// beside the Battle Pass one. On each connection it is locked until the client
// sends the 6-digit code shown on the display, so only someone who can see the
// screen can use it. There is no Bluetooth pairing: the link isn't encrypted,
// and nothing is stored on either side. Battle Pass apps never touch it.
//
// Service fcf60001-7902-4817-adcc-c0380ba2609d:
// - RX fcf60002 (write): lines, ending in '\n' or '\r': the code while locked,
//   then commands. A 0x05 byte asks for the banner (send it once notifications
//   are on: the device can't tell when the client starts listening). A 0x03
//   byte (Ctrl-C) stops the running command.
// - TX fcf60003 (notify): the output, '\n' line ends. Then one control byte:
//   0x04 after the banner and after each command (ready for the next line), or
//   0x15 after a request for the code (a wrong or expired code gets a new one).
//   Reading it gives "bx-tacho <version> <Bluetooth address>", plus " flip"
//   when the display is turned 180 degrees (cfg flip).
#pragma once

#include <stdbool.h>
#include <stdint.h>

struct ble_gap_event;

// From bbp.c while it sets up NimBLE, before the host starts
int ble_console_init(void);
// Every GAP event of the connection, from bbp.c's handler
void ble_console_gap_event(const struct ble_gap_event *event);

// True while a code waits to be entered on the other side, for the display
bool ble_console_code(uint32_t *code);

// True once the code has been entered on this connection (ble_ota.c needs it too)
bool ble_console_unlocked(void);
