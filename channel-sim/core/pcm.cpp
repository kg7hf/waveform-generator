// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

#include "core/pcm.hpp"

#include <algorithm>
#include <cmath>

namespace channel_sim
{

namespace
{
constexpr double unit_limit = 1.0;
constexpr double pcm16_lowest = -32768.0;
constexpr double pcm16_highest = 32767.0;
} // namespace

Pcm16 unit_to_pcm16_truncated(double value) noexcept
{
    if (std::isnan(value))
    {
        return 0;
    }

    const double clamped = std::max(std::min(value, unit_limit), -unit_limit);
    // |clamped * 32767| <= 32767, so the conversion is in range; it truncates toward zero.
    return static_cast<Pcm16>(clamped * pcm16_output_scale);
}

Pcm16 unit_to_pcm16_rounded(double value) noexcept
{
    if (std::isnan(value))
    {
        return 0;
    }

    const double scaled = std::clamp(value * pcm16_output_scale, pcm16_lowest, pcm16_highest);
    // lround of a value in [-32768, 32767] is in range of int16.
    return static_cast<Pcm16>(std::lround(scaled));
}

} // namespace channel_sim
