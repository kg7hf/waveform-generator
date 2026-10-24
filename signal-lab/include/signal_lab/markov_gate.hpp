// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

#pragma once

// Frame-based deterministic Markov ON/OFF gating for a signal interferer.

#include "common/status.hpp"
#include "signal_lab/det_math.hpp"

#include <cstdint>

namespace signal_lab
{

inline constexpr std::uint32_t markov_gate_algorithm_version = 1U;
inline constexpr std::uint32_t markov_gate_rng_family = 10U;
inline constexpr double markov_gate_max_mean_seconds = 1.0e6;
inline constexpr std::uint64_t markov_gate_max_mean_frames = 48000000000ULL;

struct MarkovGateParams
{
    bool enabled{};
    double mean_on_seconds{};
    double mean_off_seconds{};
};

[[nodiscard]] m110::Status validate_markov_gate_params(const MarkovGateParams& params) noexcept;

// Draws one geometric integer dwell on entry to each state, including initial ON.
// The 24-bit uniform source has quantized probabilities and a finite maximum
// draw, so dwell tails are bounded rather than continuous/unbounded; this is v1.
class MarkovGate
{
public:
    [[nodiscard]] m110::Status configure(const MarkovGateParams& params, std::uint64_t seed, std::uint32_t index) noexcept;
    [[nodiscard]] bool on() const noexcept { return mOn; }

    [[nodiscard]] std::uint64_t dwell_frames() const noexcept { return mDwellFrames; }

    [[nodiscard]] std::uint64_t dwell_elapsed() const noexcept { return mDwellElapsed; }

    // Advance after consuming the current sample. True means a new ON dwell began.
    [[nodiscard]] bool advance() noexcept;

private:
    [[nodiscard]] std::uint64_t draw_dwell(double mean_frames) noexcept;

    det::Pcg32 mGenerator{};
    double mMeanOnFrames{};
    double mMeanOffFrames{};
    std::uint64_t mDwellFrames{};
    std::uint64_t mDwellElapsed{};
    bool mOn{};
    bool mConfigured{};
};

} // namespace signal_lab
