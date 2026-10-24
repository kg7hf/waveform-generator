// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// C entry points of rt1170/usb_ep0_order.cpp for the USB interrupt's glue
// Guide: docs/modules/rt1170-player-and-audio.md#ep0-event-order
// Ownership: the USB ISR (usb/usb_tinyusb_port.c and usb/usb_ep0_order_glue.c)
// calls them, and the wfg_usb task calls wfg_usb_ep0_order_reset() once,
// before TinyUSB enables the interrupt.
// Contract: plain C values. The C++ side never sees TinyUSB types.

#pragma once

#include "platform/wfg_status.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

// A SETUP as TinyUSB's driver reported it: its root port and its packet's
// 8 bytes (USB 2.0 9.3), copied as they are (memcpy, in their wire order).
// The order holds the packet and never reads it; usb_ep0_order_glue.c checks
// that it fills this member exactly.
struct wfg_usb_setup
{
    uint8_t rhport;
    uint64_t packet;
};

// Clears the order and its counters (they live in OCRAM2, which startup
// never writes).
void wfg_usb_ep0_order_reset(void);

// The interrupt starts, before TinyUSB's first pass; it ends once every held
// SETUP is released.
void wfg_usb_ep0_order_begin(void);
void wfg_usb_ep0_order_end(void);

// A SETUP of a pass: kWFG_StatusOk when it is held. Else
// kWFG_StatusEp0OrderIdle outside an interrupt, kWFG_StatusEp0HoldFull with
// the hold full (counted in usb_ep0_unordered), or kWFG_StatusInvalidArgument
// for no SETUP: release the held ones, then pass this one on.
__attribute__((warn_unused_result)) enum wfg_status wfg_usb_ep0_order_hold(const struct wfg_usb_setup* setup);

// An EP0 completion of a pass, passed on at once (ahead of any held SETUP).
void wfg_usb_ep0_order_completion(void);

// Whether a SETUP is held.
bool wfg_usb_ep0_order_holding(void);

// The held SETUPs are released before another event of a pass; with an EP0
// completion still pending in the controller (ENDPTCOMPLETE) they may precede
// it, which counts in usb_ep0_unordered.
void wfg_usb_ep0_order_releasing(bool ep0_completion_pending);

// Writes the oldest held SETUP to *setup and returns true, or returns false
// when none is held: a poll, whose false is never a failure. setup must
// point to a SETUP to fill; a null one is the caller's defect, a critical
// error (EP0_RELEASE_WITHOUT_SETUP) that does not return.
bool wfg_usb_ep0_order_release(struct wfg_usb_setup* setup);

// After a pass: true when another is due, because a SETUP is held and the
// controller reports an EP0 completion still pending (ENDPTCOMPLETE).
bool wfg_usb_ep0_order_another_pass(bool ep0_completion_pending);

#ifdef __cplusplus
}
#endif
