// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

#pragma once

// Scenario-wide reference bandwidth for the AWGN N0 convention.

#include "signal_lab/units/frequency.hpp"

#include <cstdint>
#include <utility>

namespace signal_lab
{

enum class NoiseBandwidth : std::uint16_t
{
    khz3 = 3000U,
    khz6 = 6000U,
    khz9 = 9000U,
    khz12 = 12000U,
};

[[nodiscard]] constexpr bool valid_noise_bandwidth(NoiseBandwidth bandwidth) noexcept
{
    switch (bandwidth)
    {
    case NoiseBandwidth::khz3:
    case NoiseBandwidth::khz6:
    case NoiseBandwidth::khz9:
    case NoiseBandwidth::khz12:
        return true;
    }

    return false;
}

// For invalid enum values this returns zero; callers should check the predicate.
[[nodiscard]] constexpr units::FrequencyDelta noise_band_width(NoiseBandwidth bandwidth) noexcept
{
    return valid_noise_bandwidth(bandwidth) ? units::FrequencyDelta{static_cast<double>(std::to_underlying(bandwidth)), units::Hz} : units::FrequencyDelta{};
}

} // namespace signal_lab
