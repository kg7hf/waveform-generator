// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

#pragma once

// The global frequency offset / triangle Doppler sweep parameters.

#include "signal_lab/units/frequency.hpp"

#include <compare>
#include <cstdint>

namespace signal_lab
{

class FrequencySlope
{
public:
    constexpr FrequencySlope() noexcept = default;
    constexpr explicit FrequencySlope(double hertz_per_second) noexcept : mHertzPerSecond{hertz_per_second} {}

    [[nodiscard]] constexpr double hertz_per_second() const noexcept { return mHertzPerSecond; }

    [[nodiscard]] constexpr auto operator<=>(const FrequencySlope&) const noexcept = default;

private:
    double mHertzPerSecond{};
};

enum class FrequencyShiftMode : std::uint8_t
{
    offset,
    sweep,
};

enum class TriangleDirection : std::uint8_t
{
    ascending,
    descending,
};

// The cited phase-5 clause is +/-75 Hz; this stage accepts twice that range to
// leave configuration headroom while retaining the requested 75 Hz defaults.
inline constexpr units::FrequencyDelta frequency_shift_clause_limit{75.0, units::Hz};
inline constexpr units::FrequencyDelta frequency_shift_max_offset{150.0, units::Hz};
inline constexpr units::FrequencyDelta frequency_shift_max_amplitude{150.0, units::Hz};
inline constexpr units::FrequencyDelta frequency_shift_default_amplitude{75.0, units::Hz};
inline constexpr FrequencySlope frequency_shift_default_slope{3.5};
inline constexpr std::uint32_t frequency_shift_algorithm_version = 1U;

struct FrequencyShiftParams
{
    FrequencyShiftMode mode{FrequencyShiftMode::offset};
    units::FrequencyDelta offset{};
    units::FrequencyDelta amplitude{frequency_shift_default_amplitude};
    units::FrequencyDelta start_frequency{-75.0, units::Hz};
    FrequencySlope slope{frequency_shift_default_slope};
    TriangleDirection direction{TriangleDirection::ascending};
};

} // namespace signal_lab
