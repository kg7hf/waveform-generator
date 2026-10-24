// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// The firmware's critical-error handler: what a failed Result::value() (or
// another m110::critical_error() call) leaves for a debugger before the core
// traps
// Guide: docs/modules/rt1170-player-and-audio.md (Critical errors)
// Ownership: install_critical_error_handler() runs once, first in the
// application task, before any other task starts; record_critical_error()
// runs from any context and masks interrupts. A second call in one boot can
// come only from a context PRIMASK does not mask (NMI, HardFault): it leaves
// the record as it is.
// Contract: the record holds the first critical error of the boot, in OCRAM2
// (.ocram.critical_error) and written through the data cache to memory
// (platform_write_back()), so a debugger reads it while the core sits in
// HardFault_Handler.

#pragma once

#include "common/status.hpp"

#include <cstdint>

namespace waveform_generator
{

// A record's signature once it holds a critical error: "CRIT" in ASCII,
// read as a little-endian word.
inline constexpr std::uint32_t critical_error_signature = 0x54495243U;

// The critical error of this boot, all zero until there is one.
struct CriticalErrorRecord
{
    std::uint32_t signature{};                   // critical_error_signature once recorded
    m110::StatusCode code{m110::StatusCode::ok}; // the Status's code
    const char* message{};                       // its message, a static string
    std::uintptr_t site{};                       // the call site (m110::CriticalSite)
};

// The record of `status`, raised at `site`.
[[nodiscard]] constexpr CriticalErrorRecord critical_error_record_of(m110::Status status, m110::CriticalSite site) noexcept
{
    return {.signature = critical_error_signature, .code = status.code, .message = status.message, .site = site.call};
}

// The handler: masks interrupts (platform_mask_interrupts()), so nothing
// runs between it and the trap, and records `status` and `site` unless the
// record already holds a critical error. It returns; critical_error() then
// traps (UDF, which HardFault_Handler, a branch to itself, takes).
void record_critical_error(m110::Status status, m110::CriticalSite site) noexcept;

// Clears the record (OCRAM2 is not initialised at startup) and installs
// record_critical_error() as the handler.
void install_critical_error_handler() noexcept;

// The record as it stands. Caller: any context.
[[nodiscard]] CriticalErrorRecord critical_error_record() noexcept;

} // namespace waveform_generator
