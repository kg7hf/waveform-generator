// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

#pragma once

// Optional sample-contiguous Rayleigh envelope for the existing additive CW tone.

#include "common/status.hpp"
#include "signal_lab/units/frequency.hpp"

#include <cstdint>

namespace signal_lab
{

class WattersonPathState;

struct CwFadingParams
{
    bool enabled{};
    units::FrequencyDelta spread{};
};

inline constexpr std::uint32_t cw_fading_algorithm_version = 1U;
inline constexpr std::uint32_t cw_fading_rng_family = 12U;

[[nodiscard]] m110::Status validate_cw_fading_params(const CwFadingParams& params) noexcept;
[[nodiscard]] m110::Status configure_cw_fading(WattersonPathState& state, const CwFadingParams& params, std::uint64_t seed, std::uint32_t index) noexcept;

} // namespace signal_lab
