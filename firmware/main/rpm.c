#include "rpm.h"

#include "board.h"
#include "driver/ledc.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_pm.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sense_adc.h"

static const char *TAG = "rpm";

#define LEDC_MODE       LEDC_LOW_SPEED_MODE
#define LEDC_TIMER      LEDC_TIMER_0
#define LEDC_CH         LEDC_CHANNEL_0
#define LEDC_FREQ_HZ    100000
#define LEDC_RES        LEDC_TIMER_9_BIT       // 80 MHz / 100 kHz = 800 counts available
#define LEDC_MAX_DUTY   ((1 << 9) - 1)

// R17/C7 settle in about 5 ms
#define REF_SETTLE_MS   6

// Ring of edge timestamps, written by the ISR and drained by one task.
// 20,000 RPM with several edges per rev and a 50 ms drain interval fits easily.
#define RING_SIZE       256
#define RING_MASK       (RING_SIZE - 1)

static volatile uint32_t s_ring[RING_SIZE];
static volatile uint32_t s_head;
static volatile uint32_t s_tail;
static volatile uint32_t s_overflow;

static bool s_powered;
static int s_thr_mv = RPM_THR_DEFAULT_MV;
static esp_pm_lock_handle_t s_pm_sleep_lock;
static esp_pm_lock_handle_t s_pm_apb_lock;

static void IRAM_ATTR rpm_edge_isr(void *arg)
{
    uint32_t now = (uint32_t)esp_timer_get_time();
    uint32_t head = s_head;
    if (head - s_tail >= RING_SIZE) {
        s_overflow++;
        return;
    }
    s_ring[head & RING_MASK] = now;
    __atomic_store_n(&s_head, head + 1, __ATOMIC_RELEASE);
}

static uint32_t thr_duty(int mv)
{
    return (uint32_t)mv * (LEDC_MAX_DUTY + 1) / BOARD_VDD_MV;
}

esp_err_t rpm_init(void)
{
    // Outputs start LOW so the comparator is never back-fed
    gpio_config_t out = {
        .pin_bit_mask = BIT64(PIN_RPM_CMP_PWR) | BIT64(PIN_RPM_IR_LED),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&out), TAG, "outputs");
    gpio_set_level(PIN_RPM_IR_LED, 0);
    gpio_set_level(PIN_RPM_CMP_PWR, 0);

    // Comparator output: open-drain with R14 pull-up to RPM_CMP_PWR
    gpio_config_t in = {
        .pin_bit_mask = BIT64(PIN_RPM_IR_TRIG),
        .mode = GPIO_MODE_INPUT,
        .intr_type = GPIO_INTR_POSEDGE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&in), TAG, "trig");
    ESP_RETURN_ON_ERROR(gpio_install_isr_service(ESP_INTR_FLAG_IRAM), TAG, "isr service");
    ESP_RETURN_ON_ERROR(gpio_isr_handler_add(PIN_RPM_IR_TRIG, rpm_edge_isr, NULL), TAG, "isr");
    gpio_intr_disable(PIN_RPM_IR_TRIG);

    ledc_timer_config_t timer = {
        .speed_mode = LEDC_MODE,
        .timer_num = LEDC_TIMER,
        .duty_resolution = LEDC_RES,
        .freq_hz = LEDC_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), TAG, "ledc timer");

    ledc_channel_config_t ch = {
        .gpio_num = PIN_RPM_THR_PWM,
        .speed_mode = LEDC_MODE,
        .channel = LEDC_CH,
        .timer_sel = LEDC_TIMER,
        .duty = 0,
    };
    ESP_RETURN_ON_ERROR(ledc_channel_config(&ch), TAG, "ledc channel");
    ledc_stop(LEDC_MODE, LEDC_CH, 0);

    // The interrupt timing and the 100 kHz PWM need the clocks running
    ESP_RETURN_ON_ERROR(esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "rpm", &s_pm_sleep_lock), TAG, "pm");
    ESP_RETURN_ON_ERROR(esp_pm_lock_create(ESP_PM_APB_FREQ_MAX, 0, "rpm_apb", &s_pm_apb_lock), TAG, "pm");
    return ESP_OK;
}

void rpm_set_threshold_mv(int mv)
{
    if (mv < 0) {
        mv = 0;
    } else if (mv > RPM_THR_MAX_MV) {
        mv = RPM_THR_MAX_MV;
    }
    s_thr_mv = mv;
    if (s_powered) {
        ledc_set_duty(LEDC_MODE, LEDC_CH, thr_duty(mv));
        ledc_update_duty(LEDC_MODE, LEDC_CH);
    }
}

int rpm_get_threshold_mv(void)
{
    return s_thr_mv;
}

void rpm_power(bool on)
{
    if (on == s_powered) {
        return;
    }
    if (on) {
        esp_pm_lock_acquire(s_pm_sleep_lock);
        esp_pm_lock_acquire(s_pm_apb_lock);

        gpio_set_level(PIN_RPM_CMP_PWR, 1);
        ledc_set_duty(LEDC_MODE, LEDC_CH, thr_duty(s_thr_mv));
        ledc_update_duty(LEDC_MODE, LEDC_CH);
        vTaskDelay(pdMS_TO_TICKS(REF_SETTLE_MS));
        gpio_set_level(PIN_RPM_IR_LED, 1);

        // Drop anything left over from before, then start counting
        s_tail = __atomic_load_n(&s_head, __ATOMIC_ACQUIRE);
        gpio_intr_enable(PIN_RPM_IR_TRIG);
        s_powered = true;
    } else {
        gpio_intr_disable(PIN_RPM_IR_TRIG);
        gpio_set_level(PIN_RPM_IR_LED, 0);
        ledc_stop(LEDC_MODE, LEDC_CH, 0);
        gpio_set_level(PIN_RPM_CMP_PWR, 0);
        s_powered = false;

        esp_pm_lock_release(s_pm_apb_lock);
        esp_pm_lock_release(s_pm_sleep_lock);
    }
}

bool rpm_is_powered(void)
{
    return s_powered;
}

int rpm_read_sense_mv(int samples)
{
    return sense_adc_read_mv(ADC_CH_RPM_SENSE, samples);
}

int rpm_take_edges(uint32_t *out, int max)
{
    uint32_t head = __atomic_load_n(&s_head, __ATOMIC_ACQUIRE);
    int n = 0;
    while (s_tail != head && n < max) {
        out[n++] = s_ring[s_tail & RING_MASK];
        s_tail++;
    }
    return n;
}

uint32_t rpm_overflow_count(void)
{
    return s_overflow;
}
