// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// The host test's control of usb_descriptor_bus.c.

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C"
{
#endif

// The bus speed tud_speed_get() reports from now on (high speed at start).
void wfg_test_set_high_speed(bool high_speed);

#ifdef __cplusplus
}
#endif
