// Firmware updates over BLE, from the web app. A GATT service of its own; it
// only works while the console (ble_console.h) is unlocked with the code on the
// display. The image goes to the idle OTA slot and is checked (ESP-IDF image
// check, and the project name must be ours) before BX Tacho boots it; until
// then, and if anything fails, the current firmware stays. After the restart
// the new firmware runs on probation until it confirms itself (main.c).
//
// Service fcf60011-7902-4817-adcc-c0380ba2609d, characteristic fcf60012
// (write, write without response, notify). Each write is one message:
// - 0x01 BEGIN, u32 image size: erases room for it (a few seconds)
// - 0x02 DATA, bytes: the image, in order, up to MTU - 4 bytes per write
// - 0x03 END: check the image, make it the boot image, restart
// - 0x04 ABORT
// Notified status: op, status (ble_ota_status_t), u32 value, little-endian.
// BEGIN's value is the ATT MTU; DATA's is the number of bytes written so far,
// sent every BLE_OTA_ACK_BYTES and at the end. The client keeps at most
// BLE_OTA_WINDOW bytes beyond the last acknowledged count in flight.
#pragma once

#include <stdbool.h>

#define BLE_OTA_ACK_BYTES 4096
#define BLE_OTA_WINDOW    8192

typedef enum {
    BLE_OTA_OK,
    BLE_OTA_LOCKED,        // console not unlocked with the code
    BLE_OTA_BATTERY,       // battery at 50 % or less, and no USB power
    BLE_OTA_NO_SLOT,       // partition table without OTA slots: update over USB once
    BLE_OTA_TOO_BIG,
    BLE_OTA_FLASH,         // erasing or writing failed
    BLE_OTA_BAD_IMAGE,     // not a valid image for this chip
    BLE_OTA_NOT_OURS,      // a valid image of another project
    BLE_OTA_SEQUENCE,      // out of order, too much data, or data lost
} ble_ota_status_t;

struct ble_gap_event;

// From bbp.c while it sets up NimBLE, before the host starts
int ble_ota_init(void);
// Every GAP event of the connection, from bbp.c's handler
void ble_ota_gap_event(const struct ble_gap_event *event);

// While an update runs, for the display: percent written, and whether BX Tacho
// is about to restart into the new firmware
bool ble_ota_progress(int *percent, bool *restarting);
