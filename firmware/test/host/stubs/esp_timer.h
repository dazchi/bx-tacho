// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Darren Chi
// Host tests: the clock is fake (fake.h moves it)
#pragma once

#include <stdint.h>

int64_t esp_timer_get_time(void);
