// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// The outcome of an owned C function that can fail: ok, or the failure, named
// Guide: docs/coding-standard.md (Failure gates)
// Ownership: the owned C code under rt1170/platform and rt1170/usb returns it
// where C++ code would return an m110::Status (#50), and C++ reads it as one
// through status_of() (rt1170/c_status_map.hpp), which maps each value to a
// Status: a code and a static message.
// Contract: kWFG_StatusOk is 0 and the only success; every other value names
// one failure that the C code has, except kWFG_StatusCount, which stays last
// and is no status: it counts the values before it. A new value goes before
// it, with its row in c_status_map.hpp, whose static_asserts hold the rows to
// that count. A tag without a typedef, as waveform_msc.h's
// enum wfg_media_local_request_result: C writes enum wfg_status, C++ the bare
// name. C11 gives an enumeration no fixed underlying type, so its size is the
// target's: one byte on arm-none-eabi, whose GCC gives an enumeration the
// smallest type that holds its values (short enums, its default; the
// firmware sets no -fno-short-enums), and an int on the host. C and C++ make
// the same choice on one target, so a value crosses the boundary unchanged;
// it is returned, never laid out in memory another build reads. Each
// function that returns one is declared
// __attribute__((warn_unused_result)), C's [[nodiscard]]: GCC reports a
// result that is dropped, even one cast to void.

#pragma once

#ifdef __cplusplus
extern "C"
{
#endif

enum wfg_status
{
    kWFG_StatusOk = 0,
    /* A null pointer, or a request outside what the function takes (an MSC
       transfer's LUN, buffer, offset, size or range). */
    kWFG_StatusInvalidArgument = 1,
    /* The SD media refused the request in its state and changed nothing. */
    kWFG_StatusMediaRefused = 2,
    /* The SD media's start or initialisation failed and latched a media
       fault; the snapshot's error (wfg_media_error_t) says which step. */
    kWFG_StatusMediaFault = 3,
    /* The SD socket's host is not ready: SD_HostInit() has not readied it,
       or the card has none. */
    kWFG_StatusSdHostNotReady = 4,
    /* A USB clock did not start: the PHY's PLL or the controller's clock. */
    kWFG_StatusUsbClockFailed = 5,
    /* TinyUSB refused its start. */
    kWFG_StatusUsbStackFailed = 6,
    /* The EP0 order held no SETUP: outside the USB interrupt's passes. */
    kWFG_StatusEp0OrderIdle = 7,
    /* The EP0 order held no SETUP: its hold is full (counted as unordered). */
    kWFG_StatusEp0HoldFull = 8,
    /* No status: the number of values above. It stays last. */
    kWFG_StatusCount
};

#ifdef __cplusplus
}
#endif
