// RPM sensor channel: KEY1 reflective sensor + LMV331 comparator with a
// PWM-generated threshold (HANDOVER.md 3.6 and 6.1).
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

// LMV331 input range ends about 0.7-1 V below its 3.3 V supply
#define RPM_THR_MAX_MV      2000
#define RPM_THR_DEFAULT_MV  70

esp_err_t rpm_init(void);

// Power the channel up or down in the order required by the hardware:
// up = CMP_PWR, threshold PWM, settle, LED, interrupt; down = the reverse.
void rpm_power(bool on);
bool rpm_is_powered(void);

// Set the comparator threshold (RPM_REF). Takes effect immediately if powered.
void rpm_set_threshold_mv(int mv);
int rpm_get_threshold_mv(void);

// RPM_SENSE through the ADC. Only meaningful while the channel is powered.
int rpm_read_sense_mv(int samples);

// Copy out rising-edge timestamps (esp_timer microseconds, low 32 bits)
// captured since the last call. Returns the number copied.
int rpm_take_edges(uint32_t *out, int max);

// Edges lost because the buffer filled up between calls
uint32_t rpm_overflow_count(void);
