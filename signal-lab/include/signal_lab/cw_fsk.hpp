// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

#pragma once

// Optional frame-exact frequency-shift keying for the existing additive CW tone.

#include "common/status.hpp"
#include "signal_lab/units/frequency.hpp"

#include <cstdint>

namespace signal_lab
{

enum class FskPattern : std::uint8_t
{
    alternating,
    random,
};

struct FskParams
{
    bool enabled{};
    double baud{};
    units::FrequencyDelta shift{}; // Full mark-space separation.
    FskPattern pattern{FskPattern::alternating};
};

inline constexpr std::uint32_t cw_fsk_algorithm_version = 1U;
inline constexpr std::uint32_t cw_fsk_rng_family = 11U;
inline constexpr std::uint64_t cw_fsk_max_symbol_frames = 48000000000000ULL;

[[nodiscard]] m110::Result<std::uint64_t> cw_fsk_symbol_frames(const FskParams& params) noexcept;
[[nodiscard]] m110::Status validate_cw_fsk_params(const FskParams& params, double center_hz) noexcept;

} // namespace signal_lab
