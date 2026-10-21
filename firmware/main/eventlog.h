// Small event log in RTC memory for power/sleep debugging. It survives deep
// sleep and software/watchdog/brownout resets (not power-on), and uses the RTC
// clock so the timeline continues across deep sleep.
#pragma once

#include <stdint.h>

typedef enum {
    EV_BOOT,          // a = reset reason, b = wake-up cause bits
    EV_LOAD_ON,       // a = LOAD_SENSE mV, b = %, c = raw LED-on mV, d = raw LED-off mV,
    EV_LOAD_OFF,      //   flags: EV_FLAG_LOAD_FLASH if a fast flash sample (else a poll)
    EV_STATE,         // a = new launch state, b = old launch state
    EV_USB_ON,        // a = VBAT mV
    EV_USB_OFF,       // a = VBAT mV
    EV_DISPLAY_ON,    // a = seconds the display had been off
    EV_DISPLAY_OFF,
    EV_DEEP_SLEEP,    // a = app_sleep_reason_t, b = VBAT mV
    EV_MODE,          // a = new app mode
} ev_type_t;

// Load sample came from hold mode (LED held on, ambient measured once) rather
// than a full LED-off/on poll
#define EV_FLAG_LOAD_FLASH 0x01

// Call once at boot: validates the RTC block and records an EV_BOOT entry
void eventlog_init(void);
void eventlog_add(ev_type_t type, int a, int b);
void eventlog_add_ext(ev_type_t type, int a, int b, int c, int d, uint8_t flags);
void eventlog_print(void);
void eventlog_clear(void);
uint32_t eventlog_boot_count(void);
const char *eventlog_reset_reason_name(int reason);
