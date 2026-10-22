#include "eventlog.h"

#include <stdio.h>
#include <string.h>
#include "app.h"
#include "cli.h"
#include "esp_attr.h"
#include "esp_rtc_time.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "launch.h"

// Bump when ev_entry_t changes, so a log in the old layout is discarded
#define EV_MAGIC  0xB7AC0E02u
#define EV_LEN    48

typedef struct {
    uint32_t t_ms;      // RTC time since power-on
    uint16_t boot;
    uint8_t type;
    uint8_t flags;      // EV_FLAG_*
    int16_t a;
    int16_t b;
    int16_t c;
    int16_t d;
} ev_entry_t;

typedef struct {
    uint32_t magic;
    uint32_t boots;
    uint32_t next;      // total events written; the ring holds the last EV_LEN
    ev_entry_t ev[EV_LEN];
} ev_log_t;

static RTC_NOINIT_ATTR ev_log_t s_log;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

const char *eventlog_reset_reason_name(int reason)
{
    switch (reason) {
    case ESP_RST_POWERON:    return "power-on";
    case ESP_RST_EXT:        return "external";
    case ESP_RST_SW:         return "software";
    case ESP_RST_PANIC:      return "panic";
    case ESP_RST_INT_WDT:    return "int-wdt";
    case ESP_RST_TASK_WDT:   return "task-wdt";
    case ESP_RST_WDT:        return "wdt";
    case ESP_RST_DEEPSLEEP:  return "deep-sleep";
    case ESP_RST_BROWNOUT:   return "brownout";
    case ESP_RST_USB:        return "usb";
    case ESP_RST_JTAG:       return "jtag";
    case ESP_RST_PWR_GLITCH: return "power-glitch";
    case ESP_RST_CPU_LOCKUP: return "cpu-lockup";
    default:                 return "unknown";
    }
}

static const char *sleep_reason_name(int r)
{
    switch (r) {
    case APP_SLEEP_IDLE:     return "idle";
    case APP_SLEEP_LOW_BATT: return "low battery";
    case APP_SLEEP_COMMAND:  return "command";
    default:                 return "?";
    }
}

void eventlog_init(void)
{
    esp_reset_reason_t reason = esp_reset_reason();
    if (s_log.magic != EV_MAGIC || reason == ESP_RST_POWERON || s_log.next > 0x7FFFFFFF) {
        memset(&s_log, 0, sizeof(s_log));
        s_log.magic = EV_MAGIC;
    }
    s_log.boots++;
    eventlog_add(EV_BOOT, reason, (int)(esp_sleep_get_wakeup_causes() & 0x7FFF));
}

void eventlog_add_ext(ev_type_t type, int a, int b, int c, int d, uint8_t flags)
{
    uint32_t t_ms = (uint32_t)(esp_rtc_get_time_us() / 1000);
    taskENTER_CRITICAL(&s_lock);
    ev_entry_t *e = &s_log.ev[s_log.next % EV_LEN];
    *e = (ev_entry_t) {
        .t_ms = t_ms,
        .boot = (uint16_t)s_log.boots,
        .type = (uint8_t)type,
        .flags = flags,
        .a = (int16_t)a,
        .b = (int16_t)b,
        .c = (int16_t)c,
        .d = (int16_t)d,
    };
    s_log.next++;
    taskEXIT_CRITICAL(&s_lock);
}

void eventlog_add(ev_type_t type, int a, int b)
{
    eventlog_add_ext(type, a, b, 0, 0, 0);
}

void eventlog_clear(void)
{
    taskENTER_CRITICAL(&s_lock);
    s_log.next = 0;
    taskEXIT_CRITICAL(&s_lock);
}

uint32_t eventlog_boot_count(void)
{
    return s_log.boots;
}

// "4126 mV", or "n/a" for an event logged before the first battery reading
static const char *vbat_text(int mv, char *buf, size_t len)
{
    if (mv <= 0) {
        return "n/a";
    }
    snprintf(buf, len, "%d mV", mv);
    return buf;
}

static void print_entry(const ev_entry_t *e)
{
    char vbuf[16];
    uint32_t s = e->t_ms / 1000;
    cli_printf("%4u  %3lu:%02lu:%02lu.%01lu  ", e->boot, (unsigned long)(s / 3600),
               (unsigned long)(s / 60 % 60), (unsigned long)(s % 60), (unsigned long)(e->t_ms % 1000 / 100));

    switch (e->type) {
    case EV_BOOT:
        cli_printf("BOOT         reset=%s, wake-up causes=0x%x\n", eventlog_reset_reason_name(e->a), e->b);
        break;
    case EV_LOAD_ON:
    case EV_LOAD_OFF:
        cli_printf("%-12s LOAD_SENSE %d mV (%d%%), ON %d OFF %d mV, %s\n",
                   e->type == EV_LOAD_ON ? "LOAD ON" : "LOAD OFF", e->a, e->b, e->c, e->d,
                   (e->flags & EV_FLAG_LOAD_FLASH) ? "flash" : "poll");
        break;
    case EV_STATE:
        cli_printf("STATE        %s -> %s\n", launch_state_name(e->b), launch_state_name(e->a));
        break;
    case EV_USB_ON:
    case EV_USB_OFF:
        cli_printf("%-12s VBAT %s\n", e->type == EV_USB_ON ? "USB ON" : "USB OFF",
                   vbat_text(e->a, vbuf, sizeof(vbuf)));
        break;
    case EV_DISPLAY_ON:
        cli_printf("DISPLAY ON   after %d s off\n", e->a);
        break;
    case EV_DISPLAY_OFF:
        cli_printf("DISPLAY OFF\n");
        break;
    case EV_DEEP_SLEEP:
        cli_printf("DEEP SLEEP   reason=%s, VBAT %s\n", sleep_reason_name(e->a),
                   vbat_text(e->b, vbuf, sizeof(vbuf)));
        break;
    case EV_MODE:
        cli_printf("MODE         %s\n", app_mode_name(e->a));
        break;
    case EV_BLE_GESTURE:
        cli_printf("BLE GESTURE  BLE switched %s, restarting\n", e->a ? "on" : "off");
        break;
    default:
        cli_printf("?            %d %d\n", e->a, e->b);
        break;
    }
}

void eventlog_print(void)
{
    ev_log_t copy;
    taskENTER_CRITICAL(&s_lock);
    copy = s_log;
    taskEXIT_CRITICAL(&s_lock);

    uint32_t count = copy.next < EV_LEN ? copy.next : EV_LEN;
    cli_printf("boot  time (h:m:s, RTC)  event\n");
    for (uint32_t i = copy.next - count; i < copy.next; i++) {
        print_entry(&copy.ev[i % EV_LEN]);
    }
    if (copy.next > EV_LEN) {
        cli_printf("(%lu older events overwritten)\n", (unsigned long)(copy.next - EV_LEN));
    }
}
