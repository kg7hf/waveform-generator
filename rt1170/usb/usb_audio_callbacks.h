// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// C entry points of rt1170/usb_audio.cpp for the TinyUSB glue
// Guide: docs/modules/protocols-media-and-artifacts.md
// Ownership: usb_audio.cpp implements these; only usb_audio_tinyusb.c, the USB
// interrupt handler in usb_tinyusb_port.c (the watchdog) and the tusb_config.h
// breakpoint hook call them.
// Contract: plain C values in and out. The C++ side never sees TinyUSB types.

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "usb/usb_composite.h"

#ifdef __cplusplus
extern "C"
{
#endif

typedef enum wfg_uac_request_result
{
    kWFG_UacRequestStall = 0U,
    kWFG_UacRequestAccepted = 1U,
} wfg_uac_request_result_t;

// One class-specific entity request, decoded from the SETUP packet.
typedef struct wfg_uac_request
{
    uint8_t code;     // bRequest: wfg_uac_request_cur or wfg_uac_request_range
    uint8_t entity;   // high byte of wIndex
    uint8_t selector; // high byte of wValue: the control selector
    uint8_t channel;  // low byte of wValue: 0 is the master channel
} wfg_uac_request_t;

typedef struct wfg_uac_response
{
    wfg_uac_request_result_t result;
    uint16_t length; // valid bytes in bytes[]; TinyUSB truncates to wLength
    uint8_t bytes[wfg_uac_control_response_max_bytes];
} wfg_uac_response_t;

// Control requests. Caller: the wfg_usb task (TinyUSB control stage). A stall
// result makes TinyUSB stall the control pipe and changes nothing.
wfg_uac_response_t wfg_usb_audio_get_request(wfg_uac_request_t request);
wfg_uac_request_result_t wfg_usb_audio_set_request(wfg_uac_request_t request, const uint8_t* data, uint16_t length);

// Stream events. Caller: the USB interrupt level, meaning the USB ISR or the
// wfg_usb task inside a critical section that masks it.
// configure_stream: SET_CONFIGURATION other than 0. deconfigure_stream: bus
// reset, unplug or SET_CONFIGURATION(0); the stream closes and cannot open
// until the next configure_stream. open_stream (an alternate other than 0)
// opens the stream in that alternate's format (usb/usb_composite.h) and
// pre-fills the IN FIFO, or returns a stall result, changing nothing, outside a
// configuration or for an alternate the descriptors do not offer.
// close_stream: the open alternate is left. tx_done gets the byte count of the
// packet that just completed, the controller's 11-bit USB frame number and the
// RTOS tick, both read in the ISR; while a play is armed it pulls the player's
// file frames (player_consume_from_isr).
void wfg_usb_audio_configure_stream(void);
void wfg_usb_audio_deconfigure_stream(void);
wfg_uac_request_result_t wfg_usb_audio_open_stream(uint8_t alternate);
void wfg_usb_audio_close_stream(void);
void wfg_usb_audio_tx_done(uint16_t bytes_sent, uint16_t frame_number, uint32_t tick);

// The IN endpoint's service watchdog expired (usb/usb_tinyusb_port.h): no
// completion handled for 1.5 service intervals (one after a late completion
// or a sample that found the packet waiting). transfer_primed says whether a
// transfer was still primed on the endpoint as the USB ISR found it, before
// TinyUSB handled any completion pending with the expiry. Caller: the USB ISR.
void wfg_usb_audio_watchdog_expired(bool transfer_primed);

// TinyUSB's TU_ASSERT breakpoint hook (CFG_TUSB_DEBUG_BREAKPOINT), called by
// the TinyUSB function itself where a TU_ASSERT fails or it calls
// TU_BREAKPOINT() directly. Any context: the wfg_usb task or the USB ISR.
// Counts the failure and records its call site (the first and the
// latest), the stream frame index and the RTOS tick, under its own critical
// section, for the WFG-LIVE/1 USB? query; it neither allocates nor logs.
void wfg_usb_count_stack_assert(void);

#ifdef __cplusplus
}
#endif
