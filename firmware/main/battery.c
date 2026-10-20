#include "battery.h"

#include "board.h"
#include "esp_timer.h"
#include "sense_adc.h"
#include "vbat_cal.h"

#define VBAT_SETTLE_US  (BATTERY_SETTLE_MS * 1000LL)

esp_err_t battery_init(void)
{
    // Both inputs have external resistors: R2/R5 divider and R7 pull-up.
    gpio_config_t cfg = {
        .pin_bit_mask = BIT64(PIN_VUSB_DET) | BIT64(PIN_CHRG_DET),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    return gpio_config(&cfg);
}

bool battery_usb_present(void)
{
    return gpio_get_level(PIN_VUSB_DET) == 1;
}

int battery_percent_from_mv(int mv)
{
    // Rough LiPo discharge curve (HANDOVER.md 6.4)
    static const struct { int mv; int pct; } curve[] = {
        {3300, 0}, {3600, 10}, {3800, 50}, {4200, 100},
    };
    const int n = sizeof(curve) / sizeof(curve[0]);

    if (mv <= curve[0].mv) {
        return 0;
    }
    for (int i = 1; i < n; i++) {
        if (mv <= curve[i].mv) {
            int span_mv = curve[i].mv - curve[i - 1].mv;
            int span_pct = curve[i].pct - curve[i - 1].pct;
            return curve[i - 1].pct + (mv - curve[i - 1].mv) * span_pct / span_mv;
        }
    }
    return 100;
}

int battery_read_pin_mv(int samples)
{
    return sense_adc_read_mv(ADC_CH_VBAT, samples);
}

void battery_update(battery_status_t *out)
{
    bool usb = battery_usb_present();
    bool charging = gpio_get_level(PIN_CHRG_DET) == 0;

    out->src = !usb ? POWER_SRC_BATTERY : (charging ? POWER_SRC_CHARGING : POWER_SRC_FULL);

    if (esp_timer_get_time() < VBAT_SETTLE_US) {
        out->vbat_valid = false;
        return;
    }

    int pin_mv = battery_read_pin_mv(16);
    if (pin_mv < 0) {
        out->vbat_valid = false;
        return;
    }
    out->vbat_mv = vbat_cal_apply(pin_mv);
    // While USB is present the charger lifts the reading; only trust it on battery.
    out->vbat_valid = !usb;
    out->percent = battery_percent_from_mv(out->vbat_mv);
}
