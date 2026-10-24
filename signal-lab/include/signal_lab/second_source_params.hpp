// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

#pragma once

#include "common/status.hpp"
#include "signal_lab/analytic_filter.hpp"
#include "signal_lab/markov_gate.hpp"
#include "signal_lab/units/decibel.hpp"
#include "signal_lab/units/frequency.hpp"
#include "signal_lab/watterson_params.hpp"

#include <cstdint>

namespace signal_lab
{

inline constexpr std::uint64_t max_second_source_frames = 48000000000000ULL;
inline constexpr units::FrequencyDelta second_source_frequency_offset_limit{75.0, units::Hz};
inline constexpr std::uint32_t second_source_fading_algorithm_version = 1U;
inline constexpr std::uint32_t second_source_fading_rng_family = 13U;

struct SecondSourceFadingParams
{
    bool enabled{};
    units::FrequencyDelta spread{};
};

struct SecondSourceParams
{
    units::Ratio si{}; // Desired-signal to interferer power ratio, in dB.
    std::uint64_t start_frame{};
    std::uint64_t source_start_frame{};
    std::uint64_t duration_frames{}; // Zero means the rest of the secondary source.
    // Zero bypasses filtering unless fading is enabled. An offset or fading uses
    // one fixed-latency analytic shifter, retains input duration, and does not
    // flush its terminal tail.
    units::FrequencyDelta frequency_offset{};
    SecondSourceFadingParams fading{};
    // Gates the channel output in the destination window; upstream state continues while OFF.
    MarkovGateParams markov{};
};

[[nodiscard]] m110::Status validate_second_source_params(const SecondSourceParams& params) noexcept;
[[nodiscard]] m110::Status validate_second_source_fading_params(const SecondSourceFadingParams& params) noexcept;

[[nodiscard]] constexpr units::SampleCount second_source_filter_latency_frames(const SecondSourceParams& params) noexcept
{
    return params.frequency_offset.in(units::Hz) != 0.0 || params.fading.enabled ? hilbert_latency : units::SampleCount{};
}

} // namespace signal_lab
