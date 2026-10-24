// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

#pragma once

// Angle: a phase with its unit in the type (the shape of modem-core's Frequency).
// Guide: docs/modules/signal-lab-watterson.md, "Types and units".
//
// Storage is canonical degrees in double, the unit a scenario states a fixed
// path's phase in (phase_deg). The algebra: Angle +/- Angle and -Angle.

#include <compare>
#include <cstdint>
#include <numbers>

namespace signal_lab::units
{

enum class AngleUnit : std::uint8_t
{
    degrees,
    radians,
};

inline constexpr auto degrees = AngleUnit::degrees;
inline constexpr auto radians = AngleUnit::radians;

// A half turn: 180 degrees = pi radians, the definition of the two units.
inline constexpr double degrees_per_half_turn = 180.0;

class Angle
{
public:
    constexpr Angle() noexcept = default;

    constexpr explicit Angle(double value, AngleUnit unit) noexcept : mDegrees{unit == AngleUnit::radians ? value * (degrees_per_half_turn / std::numbers::pi) : value} {}

    [[nodiscard]] constexpr double in(AngleUnit unit) const noexcept { return unit == AngleUnit::radians ? mDegrees * (std::numbers::pi / degrees_per_half_turn) : mDegrees; }

    [[nodiscard]] constexpr auto operator<=>(const Angle&) const noexcept = default;

    [[nodiscard]] constexpr Angle operator+(Angle other) const noexcept { return Angle{mDegrees + other.mDegrees, degrees}; }

    [[nodiscard]] constexpr Angle operator-(Angle other) const noexcept { return Angle{mDegrees - other.mDegrees, degrees}; }

    [[nodiscard]] constexpr Angle operator-() const noexcept { return Angle{-mDegrees, degrees}; }

private:
    double mDegrees{};
};

} // namespace signal_lab::units
