// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

#pragma once

// Decibel<Tag>: distinct decibel types that know their own dimensional algebra.
// Guide: docs/modules/signal-lab-watterson.md, "Types and units".
//
// The design is modem-core's (modem/common/units/decibel.hpp), copied, not
// shared. A number of decibels means nothing without saying "of what", so a
// Level (absolute, dBFS), a Gain (relative: a path level, a source gain) and a
// Ratio (dimensionless: an SNR, a filter attenuation) are three types:
//   Level +/- Gain -> Level        Level - Level -> Ratio      Level +/- Ratio -> Level
//   Gain +/- Gain -> Gain          Ratio +/- Ratio -> Ratio    -Gain, -Ratio
// Level + Level, -Level and Gain +/- Ratio do not compile. modem-core allows
// Gain +/- Ratio; here a path level plus an SNR is exactly the expression to
// rule out (QA, issue #20). Storage is canonical dB in double. The linear
// conversions go through det_math (deterministic on host and board), so they are
// runtime functions: power_ratio() and amplitude_ratio() below.

#include <compare>
#include <cstdint>

namespace signal_lab::units
{

enum class LevelUnit : std::uint8_t
{
    dBFS,
};

enum class DecibelUnit : std::uint8_t
{
    dB,
};

inline constexpr auto dBFS = LevelUnit::dBFS;
inline constexpr auto dB = DecibelUnit::dB;

struct LevelTag
{
    using Unit = LevelUnit;
    static constexpr bool is_absolute = true;
};

struct GainTag
{
    using Unit = DecibelUnit;
    static constexpr bool is_absolute = false;
};

struct RatioTag
{
    using Unit = DecibelUnit;
    static constexpr bool is_absolute = false;
};

template <typename Tag> class Decibel
{
public:
    using Unit = typename Tag::Unit;

    constexpr Decibel() noexcept = default;

    // Every unit of a decibel type is the decibel itself; the tag says which kind.
    constexpr explicit Decibel(double value, Unit /*unit*/) noexcept : mDecibels{value} {}

    [[nodiscard]] static constexpr Decibel from_db(double decibels) noexcept { return Decibel{decibels, Unit{}}; }

    [[nodiscard]] constexpr double in(Unit /*unit*/) const noexcept { return mDecibels; }

    // The canonical raw form, decibels, for the algebra below.
    [[nodiscard]] constexpr double db() const noexcept { return mDecibels; }

    [[nodiscard]] constexpr auto operator<=>(const Decibel&) const noexcept = default;

    // A relative quantity adds and negates in the dB domain; an absolute level does not.
    [[nodiscard]] constexpr Decibel operator+(Decibel other) const noexcept
        requires(!Tag::is_absolute)
    {
        return from_db(mDecibels + other.mDecibels);
    }

    [[nodiscard]] constexpr Decibel operator-(Decibel other) const noexcept
        requires(!Tag::is_absolute)
    {
        return from_db(mDecibels - other.mDecibels);
    }

    [[nodiscard]] constexpr Decibel operator-() const noexcept
        requires(!Tag::is_absolute)
    {
        return from_db(-mDecibels);
    }

private:
    double mDecibels{};
};

using Level = Decibel<LevelTag>;
using Gain = Decibel<GainTag>;
using Ratio = Decibel<RatioTag>;

[[nodiscard]] constexpr Level operator+(Level level, Gain gain) noexcept
{
    return Level::from_db(level.db() + gain.db());
}

[[nodiscard]] constexpr Level operator+(Gain gain, Level level) noexcept
{
    return Level::from_db(level.db() + gain.db());
}

[[nodiscard]] constexpr Level operator-(Level level, Gain gain) noexcept
{
    return Level::from_db(level.db() - gain.db());
}

[[nodiscard]] constexpr Ratio operator-(Level upper, Level lower) noexcept
{
    return Ratio::from_db(upper.db() - lower.db());
}

[[nodiscard]] constexpr Level operator+(Level level, Ratio ratio) noexcept
{
    return Level::from_db(level.db() + ratio.db());
}

[[nodiscard]] constexpr Level operator+(Ratio ratio, Level level) noexcept
{
    return Level::from_db(level.db() + ratio.db());
}

[[nodiscard]] constexpr Level operator-(Level level, Ratio ratio) noexcept
{
    return Level::from_db(level.db() - ratio.db());
}

// 10^(dB/10): the power ratio a gain or ratio stands for (det::exp, deterministic).
[[nodiscard]] double power_ratio(Gain gain) noexcept;
[[nodiscard]] double power_ratio(Ratio ratio) noexcept;

// 10^(dB/20): the field (amplitude) form. A helper, not a type, as in modem-core:
// mixing the 10 log10 and 20 log10 forms is the classic factor-of-two error.
[[nodiscard]] double amplitude_ratio(Gain gain) noexcept;
[[nodiscard]] double amplitude_ratio(Ratio ratio) noexcept;

// 20 log10(amplitude) as a Gain, for reports (amplitude > 0).
[[nodiscard]] Gain gain_from_amplitude(double amplitude) noexcept;

} // namespace signal_lab::units
