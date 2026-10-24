// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

#pragma once

// Optional phase-continuous triangular frequency motion for an additive CW tone.

#include "common/status.hpp"
#include "signal_lab/frequency_shift_params.hpp"
#include "signal_lab/stream_rate.hpp"
#include "signal_lab/units/frequency.hpp"
#include "signal_lab/units/sampling.hpp"

#include <cstdint>

namespace signal_lab
{

inline constexpr std::uint32_t cw_sweep_algorithm_version = 1U;

struct CwSweepParams
{
    bool enabled{};
    units::Frequency low{};
    units::Frequency high{};
    FrequencySlope slope{};
};

[[nodiscard]] m110::Status validate_cw_sweep_params(const CwSweepParams& params) noexcept;

// Triangle frequency at an absolute sample index. The configured first point is
// the low endpoint, moving upward; valid parameters and a nonzero rate are required.
[[nodiscard]] units::Frequency instantaneous_cw_frequency(const CwSweepParams& params, units::SampleIndex frame, units::SampleRate rate = stream_rate) noexcept;

// Internal oscillator support shared with CwImpairment. Values use modulo-one-turn
// Q0.64 phase words; interval_average_cw_frequency splits a frame interval exactly
// when it straddles either triangle turnaround.
[[nodiscard]] std::uint64_t cw_sweep_cycle_step(const CwSweepParams& params, units::SampleRate rate = stream_rate) noexcept;
[[nodiscard]] units::Frequency cw_sweep_frequency_at_phase(const CwSweepParams& params, std::uint64_t phase_word) noexcept;
[[nodiscard]] double interval_average_cw_frequency(const CwSweepParams& params, std::uint64_t phase_word, std::uint64_t phase_step) noexcept;

} // namespace signal_lab
