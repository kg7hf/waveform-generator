// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// The CPU cycle counter (DWT CYCCNT) for C code: the C entry points of
// m110::platform_cycle_counter() and platform_cycle_counter_frequency_hz()
// (platform/board.hpp), defined in board.cpp. The counter wraps every 2^32
// cycles (4.3 s at 996 MHz). Any context; a register read.

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

uint32_t m110_platform_cycle_counter(void);
uint32_t m110_platform_cycle_counter_frequency_hz(void);

#ifdef __cplusplus
}
#endif
