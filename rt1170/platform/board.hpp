// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace m110
{

enum class PlatformStatus : std::uint8_t
{
    idle,
    running,
    success,
    failure
};

void platform_initialize() noexcept;
[[nodiscard]] std::uint32_t platform_cycle_counter() noexcept;
[[nodiscard]] std::uint32_t platform_cycle_counter_frequency_hz() noexcept;
void platform_set_status(PlatformStatus status) noexcept;
void platform_delay(std::uint32_t milliseconds) noexcept;
void platform_idle() noexcept;
void platform_write_diagnostic(const char* text) noexcept;
// Masks every interrupt of configurable priority (PRIMASK), for good: for a
// path that ends in a trap (critical_error.cpp).
void platform_mask_interrupts() noexcept;
// Writes the data cache's lines that hold `bytes` to memory: over cached
// OCRAM, for a record a debugger reads from memory.
void platform_write_back(std::span<std::byte> bytes) noexcept;

} // namespace m110
