// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Paul R. Decker

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

// Board/controller hardware is initialized before the USB task is created.
// TinyUSB stack lifecycle remains exclusively owned by that task.
bool m110_usb_hardware_initialize(void);
bool m110_tinyusb_stack_initialize(void);
void m110_tinyusb_task(uint32_t timeout_ms);
void m110_tinyusb_stack_deinitialize(void);
uint32_t m110_tinyusb_cdc_available(void);
uint32_t m110_tinyusb_cdc_read(void* buffer, uint32_t size);
uint32_t m110_tinyusb_cdc_write(const void* buffer, uint32_t size);
void m110_tinyusb_cdc_flush(void);
bool m110_tinyusb_cdc_connected(void);

// Debugger-visible bring-up progress. Values are stable evidence codes for the
// board ports and do not participate in protocol behavior.
extern volatile uint32_t g_m110_usb_port_status;

#ifdef __cplusplus
}
#endif
