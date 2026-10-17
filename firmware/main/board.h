// Pin map for the BX Tacho v0.2 board (ESP32-C3-WROOM-02).
// See HANDOVER.md section 3.9.
#pragma once

#include "driver/gpio.h"
#include "hal/adc_types.h"

// Power and charger monitoring
#define PIN_VBAT_SENSE      GPIO_NUM_0   // ADC1_CH0, VBAT / 2 (R3/R9 470k/470k)
#define PIN_VUSB_DET        GPIO_NUM_1   // HIGH = USB present, deep-sleep wake source
#define PIN_CHRG_DET        GPIO_NUM_2   // LOW = charging (TP4054 CHRG, R7 pull-up), strap pin

// RPM channel (KEY1 + LMV331)
#define PIN_RPM_SENSE       GPIO_NUM_3   // ADC1_CH3, phototransistor emitter, valid only while powered
#define PIN_RPM_IR_TRIG     GPIO_NUM_5   // comparator output, rising-edge interrupt
#define PIN_RPM_THR_PWM     GPIO_NUM_6   // 100 kHz LEDC, RC-filtered to RPM_REF
#define PIN_RPM_CMP_PWR     GPIO_NUM_10  // supplies comparator, pull-up and KEY1 collector
#define PIN_RPM_IR_LED      GPIO_NUM_21  // KEY1 LED (U0TXD, flickers with the ROM boot log)

// Load-detect channel (KEY2)
#define PIN_LOAD_SENSE      GPIO_NUM_4   // ADC1_CH4
#define PIN_LOAD_IR_LED     GPIO_NUM_7   // KEY2 LED

// OLED (SSD1315, I2C). IO8/IO9 are strap pins held high by R21/R22.
#define PIN_OLED_SDA        GPIO_NUM_8
#define PIN_OLED_SCL        GPIO_NUM_9   // shared with the BOOT button (SW3)

#define ADC_CH_VBAT         ADC_CHANNEL_0
#define ADC_CH_RPM_SENSE    ADC_CHANNEL_3
#define ADC_CH_LOAD_SENSE   ADC_CHANNEL_4

// Supply the threshold PWM is referenced to
#define BOARD_VDD_MV        3300
