// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

#pragma once

// Duration: a span of time with its unit in the type.
// Guide: docs/modules/signal-lab-watterson.md, "Types and units".
//
// modem-core has no duration type; this one has the shape of its Frequency.
// Storage is canonical milliseconds in double, the unit HF channel delays are
// stated in (MIL-STD-188-110D D.6.2, Table D-LXV; ITU-R F.1487), so a scenario's
// delay_ms is stored exactly and the frame count of E.5.2 is formed from it with
// a single rounding (to_frames in units/conversions.hpp). The algebra:
// Duration +/- Duration, Duration * or / scalar, Duration / Duration -> double.

#include <compare>
#include <cstdint>

namespace signal_lab::units
{

enum class TimeUnit : std::uint8_t
{
    s,
    ms,
    us,
};

inline constexpr auto s = TimeUnit::s;
inline constexpr auto ms = TimeUnit::ms;
inline constexpr auto us = TimeUnit::us;

// Milliseconds per second and microseconds per millisecond: the definitions of the prefixes.
inline constexpr double milliseconds_per_second = 1000.0;
inline constexpr double microseconds_per_millisecond = 1000.0;

class Duration
{
public:
    constexpr Duration() noexcept = default;

    constexpr explicit Duration(double value, TimeUnit unit) noexcept : mMilliseconds{to_milliseconds(value, unit)} {}

    [[nodiscard]] static constexpr Duration from_ms(double milliseconds) noexcept { return Duration{milliseconds, ms}; }

    [[nodiscard]] constexpr double in(TimeUnit unit) const noexcept
    {
        switch (unit)
        {
        case TimeUnit::s:
            return mMilliseconds / milliseconds_per_second;

        case TimeUnit::us:
            return mMilliseconds * microseconds_per_millisecond;

        case TimeUnit::ms:
            break;
        }

        return mMilliseconds;
    }

    [[nodiscard]] constexpr auto operator<=>(const Duration&) const noexcept = default;

    [[nodiscard]] constexpr Duration operator+(Duration other) const noexcept { return from_ms(mMilliseconds + other.mMilliseconds); }

    [[nodiscard]] constexpr Duration operator-(Duration other) const noexcept { return from_ms(mMilliseconds - other.mMilliseconds); }

    [[nodiscard]] constexpr Duration operator*(double scale) const noexcept { return from_ms(mMilliseconds * scale); }

    [[nodiscard]] constexpr Duration operator/(double divisor) const noexcept { return from_ms(mMilliseconds / divisor); }

    [[nodiscard]] constexpr double operator/(Duration other) const noexcept { return mMilliseconds / other.mMilliseconds; }

private:
    [[nodiscard]] static constexpr double to_milliseconds(double value, TimeUnit unit) noexcept
    {
        switch (unit)
        {
        case TimeUnit::s:
            return value * milliseconds_per_second;

        case TimeUnit::us:
            return value / microseconds_per_millisecond;

        case TimeUnit::ms:
            break;
        }

        return value;
    }

    double mMilliseconds{};
};

} // namespace signal_lab::units
