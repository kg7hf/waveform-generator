// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// The firmware's critical-error handler (critical_error.hpp)
// Guide: docs/modules/rt1170-player-and-audio.md (Critical errors)

#include "critical_error.hpp"

#include "platform/board.hpp"

#include <span>

namespace waveform_generator
{
namespace
{

// In OCRAM2 (.ocram, NOLOAD: startup never writes it, so its initializer is
// not loaded), out of DTCM: install_critical_error_handler() writes it before
// anything reads it.
[[nodiscard]] CriticalErrorRecord& record() noexcept
{
    __attribute__((section(".ocram.critical_error"))) static constinit CriticalErrorRecord instance{};
    return instance;
}

// Sets the record and writes it through the data cache to OCRAM2, which the
// cache holds write-back (BOARD_ConfigMPU): a debugger reads memory.
void write_record(const CriticalErrorRecord& written) noexcept
{
    CriticalErrorRecord& stored = record();
    stored = written;
    m110::platform_write_back(std::as_writable_bytes(std::span{&stored, 1U}));
}

} // namespace

void record_critical_error(m110::Status status, m110::CriticalSite site) noexcept
{
    m110::platform_mask_interrupts();

    // The first critical error of the boot stays: a later one, from a context
    // PRIMASK does not mask, would hide the cause behind a consequence.
    if (record().signature != critical_error_signature)
    {
        write_record(critical_error_record_of(status, site));
    }
}

void install_critical_error_handler() noexcept
{
    write_record(CriticalErrorRecord{});
    m110::set_critical_error_handler(&record_critical_error);
}

CriticalErrorRecord critical_error_record() noexcept
{
    return record();
}

} // namespace waveform_generator
