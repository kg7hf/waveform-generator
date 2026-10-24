// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// The EP0 race of #32 on the pinned TinyUSB, for tests/usb_ep0_order_tests.cpp
// Guide: docs/modules/rt1170-player-and-audio.md#ep0-event-order
// Contract: usb_ep0_race.c stands in for the ChipIdea driver below TinyUSB's
// real device stack (usbd.c) and CDC driver. It enumerates once, then plays
// one control transfer whose last stage completes on the bus just before the
// host's next SETUP, as the driver's interrupt reports them: the SETUP in
// one interrupt and the completion in the next (TinyUSB as pinned), or both
// through the EP0 order (usb/usb_ep0_order_glue.c), which finds the
// completion pending after the pass that reported the SETUP. Plain C values;
// the C++ test sees no TinyUSB type.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

struct wfg_race_result
{
    uint32_t asserts;             // TinyUSB assertion failures during the race
    uint32_t line_state_calls;    // tud_cdc_line_state_cb calls during it
    bool dtr;                     // the CDC driver's DTR after it
    uint32_t line_coding_calls;   // tud_cdc_line_coding_cb calls during it
    uint32_t line_coding_bitrate; // the bit rate the driver holds after it
    uint32_t ep0_primes;          // EP0 transfers TinyUSB queued during it
};

// Initialises TinyUSB and the EP0 order, then enumerates: bus reset,
// SET_CONFIGURATION(1) and DTR on, each acknowledged in order.
bool wfg_race_enumerate(void);

// SET_CONTROL_LINE_STATE (DTR off, status IN), then GET_LINE_CODING (IN
// data), the first's status ZLP completing just before the second's SETUP;
// through the EP0 order or as the pinned driver reports them.
struct wfg_race_result wfg_race_in_data(bool through_ep0_order);

// GET_LINE_CODING (status OUT), then SET_LINE_CODING (OUT data), likewise.
struct wfg_race_result wfg_race_out_data(bool through_ep0_order);

// As wfg_race_in_data(true), with SOF interrupts on: the pass that reports
// the SETUP ends with a SOF, before the extra pass reports the completion.
struct wfg_race_result wfg_race_in_data_with_sof(void);

// One interrupt's sample events through the glue, as the text of what it
// passed on, in order: "S<bRequest>" a SETUP, "C<ep>" a completion from the
// interrupt and "c<ep>" one from task context (two hex digits each), "F" a
// SOF, "R" a bus reset, separated by spaces. Writes at most `capacity`
// bytes, its NUL included.
void wfg_race_route_sample(char* text, size_t capacity);

// One interrupt whose driver reports a SETUP more than the hold keeps (each
// SETUP's bRequest its number, 1 first), through the glue, as the text of
// what it passed on (as wfg_race_route_sample()).
void wfg_race_route_full_hold(char* text, size_t capacity);

#ifdef __cplusplus
}
#endif
