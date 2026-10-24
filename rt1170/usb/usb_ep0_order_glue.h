// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// The USB interrupt's routing of TinyUSB's DCD events through the EP0 order
// Guide: docs/modules/rt1170-player-and-audio.md#ep0-event-order
// Ownership: usb_tinyusb_port.c, which interposes on dcd_event_handler (GNU
// ld --wrap, rt1170/CMakeLists.txt) and runs the interrupt's passes; the host
// test (tests/usb_ep0_order_tests.cpp) drives TinyUSB through it the same way.
// Contract: `forward` is TinyUSB's own dcd_event_handler, and
// `ep0_completion_pending` reads the controller's ENDPTCOMPLETE for EP0.
// Events from task context pass straight through; in the interrupt, SETUPs
// wait in the order (usb/usb_ep0_order.h), and EP0 completions and SOFs
// overtake them.

#pragma once

#include <stdbool.h>

#include "tusb.h"
#include "device/dcd.h"

#ifdef __cplusplus
extern "C"
{
#endif

// One event as the driver reported it. `ep0_completion_pending` is asked only
// while a SETUP is held, before another event releases it.
void wfg_usb_ep0_route_event(dcd_event_t const* event, bool in_isr, void (*forward)(dcd_event_t const* event, bool in_isr),
                             bool (*ep0_completion_pending)(void));

// Passes the held SETUPs on, oldest first: at the end of the interrupt, and
// before any event that must keep its place after them.
void wfg_usb_ep0_release_setups(void (*forward)(dcd_event_t const* event, bool in_isr));

#ifdef __cplusplus
}
#endif
