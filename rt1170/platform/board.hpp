#pragma once

#include <cstdint>

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

} // namespace m110
