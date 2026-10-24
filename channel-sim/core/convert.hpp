// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// The chain core's numeric conversions, each written once (guide:
// docs/modules/pathsim.md; C++ Core Guidelines ES.46, ES.48, ES.49).
//
// The core mixes sample counts (std::uint64_t), signed sample indices
// (std::int64_t, as the lane tool computes them) and doubles. These helpers are
// the only casts between them. Counts and indices stay at or below
// max_sample_count (2^56, about 285,000 years of 8 kHz audio): LaneResampler::push
// refuses more than 2^56 / 24 inputs, so even its outputs at the 24x maximum
// ratio stay below it. So every conversion here is exact in use, and the index
// conversions also saturate, so no value makes one wrap or be undefined.

#pragma once

#include "core/units.hpp"

#include <algorithm>
#include <cmath>
#include <concepts>
#include <cstdint>

namespace channel_sim
{

inline constexpr std::uint64_t max_sample_count = std::uint64_t{1} << 56U;
inline constexpr std::int64_t max_sample_index = std::int64_t{1} << 56;

// An integer count or index as a double: exact below 2^53, the nearest double
// above (the lane tool's double(N)).
template <std::integral Integer> [[nodiscard]] constexpr double as_double(Integer value) noexcept
{
    return static_cast<double>(value);
}

// A rate in Hz as the lane tool's double(rate).
[[nodiscard]] constexpr double as_double(SampleRate rate) noexcept
{
    return as_double(rate.hz);
}

// A count or index as a signed index, saturating at max_sample_index.
[[nodiscard]] constexpr std::int64_t to_signed_index(std::uint64_t count) noexcept
{
    return static_cast<std::int64_t>(std::min(count, max_sample_count));
}

// A signed index as an unsigned one, saturating to 0 .. max_sample_count. The
// callers have already excluded negative indices.
[[nodiscard]] constexpr std::uint64_t to_unsigned_index(std::int64_t index) noexcept
{
    return static_cast<std::uint64_t>(std::clamp(index, std::int64_t{0}, max_sample_index));
}

// floor(position) as a signed index, the lane tool's int64(floor(p)) for
// 0 <= p <= 2^56. NaN and negative positions give 0, larger ones max_sample_index.
[[nodiscard]] inline std::int64_t floor_to_index(double position) noexcept
{
    const double floored = std::floor(position);

    if (!(floored >= 0.0))
    {
        return 0;
    }

    return floored >= as_double(max_sample_index) ? max_sample_index : static_cast<std::int64_t>(floored);
}

static_assert(to_signed_index(max_sample_count + 1U) == max_sample_index, "signed indices saturate");
static_assert(to_unsigned_index(-1) == 0U, "negative indices saturate to 0");
static_assert(to_unsigned_index(max_sample_index) == max_sample_count, "the two limits are the same number");

} // namespace channel_sim
