#include "sense_adc.h"

#include "board.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "adc";

// 12 dB attenuation: about 0-2.5 V calibrated range on the ESP32-C3.
// VBAT/2 tops out at 2.1 V; the threshold is capped at 2.0 V.
#define ATTEN ADC_ATTEN_DB_12

static const adc_channel_t s_channels[] = {ADC_CH_VBAT, ADC_CH_RPM_SENSE, ADC_CH_LOAD_SENSE};
#define NUM_CH (sizeof(s_channels) / sizeof(s_channels[0]))

static adc_oneshot_unit_handle_t s_unit;
static adc_cali_handle_t s_cali[NUM_CH];
static SemaphoreHandle_t s_lock;

static int ch_index(adc_channel_t ch)
{
    for (int i = 0; i < NUM_CH; i++) {
        if (s_channels[i] == ch) {
            return i;
        }
    }
    return -1;
}

esp_err_t sense_adc_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_NO_MEM, TAG, "mutex");

    adc_oneshot_unit_init_cfg_t unit_cfg = {.unit_id = ADC_UNIT_1};
    ESP_RETURN_ON_ERROR(adc_oneshot_new_unit(&unit_cfg, &s_unit), TAG, "unit");

    adc_oneshot_chan_cfg_t ch_cfg = {.atten = ATTEN, .bitwidth = ADC_BITWIDTH_DEFAULT};
    for (int i = 0; i < NUM_CH; i++) {
        ESP_RETURN_ON_ERROR(adc_oneshot_config_channel(s_unit, s_channels[i], &ch_cfg), TAG, "chan %d", i);

        adc_cali_curve_fitting_config_t cali_cfg = {
            .unit_id = ADC_UNIT_1,
            .chan = s_channels[i],
            .atten = ATTEN,
            .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        if (adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_cali[i]) != ESP_OK) {
            ESP_LOGW(TAG, "no eFuse calibration for ch%d, readings are approximate", s_channels[i]);
            s_cali[i] = NULL;
        }
    }
    return ESP_OK;
}

int sense_adc_read_mv(adc_channel_t ch, int samples)
{
    int idx = ch_index(ch);
    if (idx < 0 || samples < 1) {
        return -1;
    }

    int sum = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < samples; i++) {
        int raw = 0;
        int mv = 0;
        if (adc_oneshot_read(s_unit, ch, &raw) != ESP_OK) {
            xSemaphoreGive(s_lock);
            return -1;
        }
        if (s_cali[idx] == NULL || adc_cali_raw_to_voltage(s_cali[idx], raw, &mv) != ESP_OK) {
            mv = raw * 2500 / 4095;
        }
        sum += mv;
    }
    xSemaphoreGive(s_lock);
    return sum / samples;
}
