// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

#pragma once

// Frequency: a quantity that carries its unit and its algebra in the type.
// Guide: docs/modules/signal-lab-watterson.md, "Types and units".
//
// The design is modem-core's (modem/common/units/frequency.hpp), copied, not
// shared. Storage is canonical hertz in double: a frequency is configuration
// (a band edge, a tone, a spread), never a per-sample value. Two kinds split by
// algebra: an absolute Frequency is a point (a band edge, a tone), a
// FrequencyDelta is a signed difference (a Doppler spread, a shift, a
// bandwidth, a node rate). The operators encode the affine space:
//   Frequency - Frequency -> FrequencyDelta
//   Frequency +/- FrequencyDelta -> Frequency
//   FrequencyDelta +/- FrequencyDelta -> FrequencyDelta
//   FrequencyDelta * or / scalar -> FrequencyDelta, FrequencyDelta / FrequencyDelta -> double
// Frequency + Frequency, scaling a Frequency and comparing across kinds do not compile.

#include "signal_lab/convert.hpp"

#include <compare>
#include <concepts>
#include <cstdint>
#include <utility>

namespace signal_lab::units
{

// The value of each unit is its size in hertz.
enum class FrequencyUnit : std::uint16_t
{
    Hz = 1,
    kHz = 1000,
};

inline constexpr auto Hz = FrequencyUnit::Hz;
inline constexpr auto kHz = FrequencyUnit::kHz;

struct AbsoluteFrequencyTag
{
};

struct FrequencyDifferenceTag
{
};

template <typename Kind> class BasicFrequency
{
public:
    constexpr BasicFrequency() noexcept = default;

    constexpr explicit BasicFrequency(double value, FrequencyUnit unit) noexcept : mHertz{value * as_double(std::to_underlying(unit))} {}

    // Canonical hertz, already in canonical form; the algebra builds its results this way.
    [[nodiscard]] static constexpr BasicFrequency from_hz(double hertz) noexcept { return BasicFrequency{hertz, RawTag{}}; }

    [[nodiscard]] constexpr double in(FrequencyUnit unit) const noexcept { return mHertz / as_double(std::to_underlying(unit)); }

    // The canonical raw form, hertz, for the algebra below.
    [[nodiscard]] constexpr double hertz() const noexcept { return mHertz; }

    [[nodiscard]] constexpr auto operator<=>(const BasicFrequency&) const noexcept = default;

    // Scale and negate exist only for a difference: an offset has a true zero, a point does not.
    [[nodiscard]] constexpr BasicFrequency operator-() const noexcept
        requires std::same_as<Kind, FrequencyDifferenceTag>
    {
        return from_hz(-mHertz);
    }

    [[nodiscard]] constexpr BasicFrequency operator*(double scale) const noexcept
        requires std::same_as<Kind, FrequencyDifferenceTag>
    {
        return from_hz(mHertz * scale);
    }

    [[nodiscard]] constexpr BasicFrequency operator/(double divisor) const noexcept
        requires std::same_as<Kind, FrequencyDifferenceTag>
    {
        return from_hz(mHertz / divisor);
    }

private:
    struct RawTag
    {
    };

    constexpr BasicFrequency(double hertz, RawTag /*raw*/) noexcept : mHertz{hertz} {}

    double mHertz{};
};

using Frequency = BasicFrequency<AbsoluteFrequencyTag>;
using FrequencyDelta = BasicFrequency<FrequencyDifferenceTag>;

[[nodiscard]] constexpr FrequencyDelta operator-(Frequency upper, Frequency lower) noexcept
{
    return FrequencyDelta::from_hz(upper.hertz() - lower.hertz());
}

[[nodiscard]] constexpr Frequency operator+(Frequency base, FrequencyDelta delta) noexcept
{
    return Frequency::from_hz(base.hertz() + delta.hertz());
}

[[nodiscard]] constexpr Frequency operator+(FrequencyDelta delta, Frequency base) noexcept
{
    return Frequency::from_hz(base.hertz() + delta.hertz());
}

[[nodiscard]] constexpr Frequency operator-(Frequency base, FrequencyDelta delta) noexcept
{
    return Frequency::from_hz(base.hertz() - delta.hertz());
}

[[nodiscard]] constexpr FrequencyDelta operator+(FrequencyDelta first, FrequencyDelta second) noexcept
{
    return FrequencyDelta::from_hz(first.hertz() + second.hertz());
}

[[nodiscard]] constexpr FrequencyDelta operator-(FrequencyDelta first, FrequencyDelta second) noexcept
{
    return FrequencyDelta::from_hz(first.hertz() - second.hertz());
}

// A pure ratio, such as a node rate over a spread (the tap-rate multiple K).
[[nodiscard]] constexpr double operator/(FrequencyDelta numerator, FrequencyDelta denominator) noexcept
{
    return numerator.hertz() / denominator.hertz();
}

[[nodiscard]] constexpr FrequencyDelta operator*(double scale, FrequencyDelta delta) noexcept
{
    return FrequencyDelta::from_hz(delta.hertz() * scale);
}

[[nodiscard]] constexpr FrequencyDelta abs(FrequencyDelta delta) noexcept
{
    return delta < FrequencyDelta{} ? -delta : delta;
}

} // namespace signal_lab::units
