#pragma once

#include <cstdint>

namespace waveform_generator
{

// WFG/1 logical headphone setting. Zero is the WM8960 mute code. Values 1..100
// span the driver's usable 0x30..0x7f range using round-to-nearest mapping.
[[nodiscard]] constexpr std::uint8_t headphone_percent_to_raw(
    std::uint8_t percent) noexcept
{
    return percent == 0U
               ? 0U
               : static_cast<std::uint8_t>(
                     48U + ((static_cast<std::uint32_t>(percent - 1U) * 79U +
                             49U) /
                            99U));
}

static_assert(headphone_percent_to_raw(0U) == 0x00U);
static_assert(headphone_percent_to_raw(1U) == 0x30U);
static_assert(headphone_percent_to_raw(70U) == 0x67U);
static_assert(headphone_percent_to_raw(100U) == 0x7fU);

} // namespace waveform_generator
