// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Paul R. Decker

#pragma once

// 300-3300 Hz band-pass FIR shared by the AWGN stage and noise-ringing crashes.
// Same design as the Phase 1 Python engine (Blackman-windowed sinc, 513 taps,
// L2-normalised so unit-variance white input gives unit output variance),
// applied as a direct symmetric FIR so the host and the RT1170 sum the same
// products in the same order.

#include "signal_lab/det_math.hpp"

#include <cstddef>
#include <cstdint>

namespace signal_lab
{

inline constexpr std::size_t band_fir_taps = 513U;
inline constexpr std::size_t band_fir_history = band_fir_taps - 1U;
inline constexpr std::size_t band_fir_half = band_fir_taps / 2U;

// Kernel computed once with the deterministic sin/cos; identical on every target.
const float* band_fir_kernel() noexcept;

class BandFir
{
public:
    // stationary: prime the history with unit Gaussian samples from rng (pcm_stress convention).
    void reset(det::Pcg32* rng, bool stationary) noexcept;

    // Filter `count` excitation samples appended after the retained history.
    // `scratch` must hold band_fir_history + count floats; output may alias input.
    void filter(const float* input, float* output, std::size_t count, float* scratch) noexcept;

private:
    float history_[band_fir_history] {};
};

} // namespace signal_lab
