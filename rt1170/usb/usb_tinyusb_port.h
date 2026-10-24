// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

#pragma once

#include "platform/wfg_status.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

// Board/controller hardware is initialized before the USB task is created.
// TinyUSB stack lifecycle remains exclusively owned by that task.
// The hardware's start: kWFG_StatusOk, or kWFG_StatusUsbClockFailed when the
// PHY's PLL or the controller's clock did not start (the PHY is then left
// alone). The stack's start: kWFG_StatusOk, or kWFG_StatusUsbStackFailed when
// TinyUSB refused it. Each leaves its bring-up status for a debugger.
__attribute__((warn_unused_result)) enum wfg_status m110_usb_hardware_initialize(void);
__attribute__((warn_unused_result)) enum wfg_status m110_tinyusb_stack_initialize(void);
void m110_tinyusb_task(uint32_t timeout_ms);
void m110_tinyusb_stack_deinitialize(void);
uint32_t m110_tinyusb_cdc_available(void);
uint32_t m110_tinyusb_cdc_read(void* buffer, uint32_t size);
uint32_t m110_tinyusb_cdc_write(const void* buffer, uint32_t size);
void m110_tinyusb_cdc_flush(void);
bool m110_tinyusb_cdc_connected(void);

// The UAC2 IN FIFO of the player's composite device, in bytes. Queued and free
// are zero while the audio function is not mounted. TinyUSB's FIFO overwrites
// when full, so a writer checks the free space first. Callers: the USB ISR, or
// a task inside a critical section that masks it (rt1170/usb_audio.cpp).
uint16_t m110_tinyusb_audio_in_queued_bytes(void);
uint16_t m110_tinyusb_audio_in_free_bytes(void);
uint16_t m110_tinyusb_audio_in_write(const void* bytes, uint16_t count);

// The 11-bit USB frame number of the last SOF the controller received
// (FRINDEX[13:3]; FRINDEX[2:0] is the microframe at high speed). One frame per
// millisecond at both speeds. Any context; a register read.
uint16_t m110_usb_frame_number(void);

// The UAC2 IN endpoint's service watchdog (player image), which tells a missed
// 1 ms interval the device caused from one the host caused. It is the USB
// controller's general-purpose timer 0 (1 us steps; TinyUSB does not use it),
// run one-shot from each IN completion's ISR: it samples the endpoint 1.5
// service intervals later, or one interval later when that ISR ran at or past
// its own deadline, and then once per interval for as long as the packet waits.
// The deadline runs from when the ISR ran, not from the host's poll, so the
// watchdog expires once per millisecond of a host pause, and also whenever the
// USB ISR is held off by about half an interval or more, even when no interval
// is missed. Its interrupt shares the USB IRQ. The handler, before TinyUSB sees
// the interrupt, acknowledges the expiry and reports whether a transfer is
// primed on the endpoint (ENDPTSTAT), through wfg_usb_audio_watchdog_expired().
// A completion pending with the expiry is then still unhandled, so the packet
// after it is not primed yet. TIE0 is hidden from TinyUSB's handler, which
// would otherwise acknowledge an expiry that came while it ran.
// start: an alternate with a format opened the stream (the wfg_usb task, USB IRQ masked);
// stops the timer and enables its interrupt in USBINTR, which otherwise only
// the USB ISR (TIE0, cleared and restored around TinyUSB's handler) and
// TinyUSB write. restart: every IN completion (the USB ISR, after TinyUSB
// primed the next packet). stop: the stream closed or the configuration ended
// (any context that masks the USB IRQ, or the ISR itself).
void m110_usb_audio_watchdog_start(void);
void m110_usb_audio_watchdog_restart(void);
void m110_usb_audio_watchdog_stop(void);

#ifdef __cplusplus
}
#endif
