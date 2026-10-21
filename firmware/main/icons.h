// 1-bit bitmaps for the OLED, drawn with display_bitmap().
// Row-major, MSB = leftmost pixel, rows padded to whole bytes.
#pragma once

#include <stdint.h>

#define ICON_BATTERY_OFF_W 48
#define ICON_BATTERY_OFF_H 48
extern const uint8_t icon_battery_off_48[];

#define ICON_BEYBLADE_W 40
#define ICON_BEYBLADE_H 40
extern const uint8_t icon_beyblade_40[];

// Status-line battery: the outline plus charging and charge-complete variants, same size
#define ICON_BATTERY_W 16
#define ICON_BATTERY_H 9
extern const uint8_t icon_battery_16x9[];
extern const uint8_t icon_battery_charging_16x9[];
extern const uint8_t icon_battery_full_16x9[];
// Level bar area inside icon_battery_16x9, relative to the icon's top-left
#define ICON_BATTERY_FILL_X 2
#define ICON_BATTERY_FILL_Y 2
#define ICON_BATTERY_FILL_W 10
#define ICON_BATTERY_FILL_H 5

// Status-line BLE state, after react-icons FaBluetoothB, MdOutlineBluetoothConnected
// and LuBluetoothOff, redrawn by hand at 9 px
#define ICON_BLE_H 9
#define ICON_BLE_ON_W 5
extern const uint8_t icon_ble_on_5x9[];
#define ICON_BLE_CONNECTED_W 9
extern const uint8_t icon_ble_connected_9x9[];
#define ICON_BLE_OFF_W 9
extern const uint8_t icon_ble_off_9x9[];
