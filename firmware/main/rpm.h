// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Darren Chi
// RPM sensor channel: KEY1 reflective sensor + LMV331 comparator with a
// PWM-generated threshold (HANDOVER.md 3.6 and 6.1).
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// LMV331 input range ends about 0.7-1 V below its 3.3 V supply
#define RPM_THR_MAX_MV      2000
#define RPM_THR_DEFAULT_MV  80
#define RPM_THR_OFFSET_MV   10

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

// Notify `task` (xTaskNotifyGive) on every edge, so it can act right after one
void rpm_set_notify_task(TaskHandle_t task);
// Call `hook` from the interrupt on every unmasked edge, with its timestamp
// (IRAM: it may run while the flash cache is off)
void rpm_set_edge_hook(void (*hook)(uint32_t edge_us));

// Edge masking around a load LED flash, whose light can reach the RPM sensor:
// edges from rpm_mask_begin() until tail_us after rpm_mask_end() are dropped.
void rpm_mask_begin(void);
void rpm_mask_end(uint32_t tail_us);
uint32_t rpm_masked_count(void);

// The last unmasked edge since the channel was powered, and the shortest of the
// (up to three) intervals between the edges before it. Returns how many edges
// that is based on: 0 (nothing valid), 1 (only *last), up to 4.
int rpm_recent_edges(uint32_t *last, uint32_t *shortest);
