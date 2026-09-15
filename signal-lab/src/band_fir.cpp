// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Paul R. Decker

#include "band_fir.hpp"

#include <cmath>

namespace signal_lab
{
namespace
{

constexpr double sample_rate = 48000.0;
constexpr double band_low_hz = 300.0;
constexpr double band_high_hz = 3300.0;

double det_sinc(double x) noexcept
{
    if (x == 0.0)
    {
        return 1.0;
    }

    const double px = det::pi * x;
    return det::sin(px) / px;
}

float kernel_storage[band_fir_taps] {};
double kernel_scratch[band_fir_taps] {}; // static: 4 KiB must not land on an embedded task stack
bool kernel_ready{};

void build_kernel() noexcept
{
    double* kernel = kernel_scratch;
    double energy = 0.0;
    const double centre = static_cast<double>(band_fir_taps - 1U) / 2.0;
    const double high = 2.0 * band_high_hz / sample_rate;
    const double low = 2.0 * band_low_hz / sample_rate;

    for (std::size_t index = 0U; index < band_fir_taps; ++index)
    {
        const double m = static_cast<double>(index) - centre;
        const double ideal = high * det_sinc(high * m) - low * det_sinc(low * m);
        // NumPy blackman(N): 0.42 - 0.5 cos(2 pi n/(N-1)) + 0.08 cos(4 pi n/(N-1))
        const double phase = det::two_pi * static_cast<double>(index) / static_cast<double>(band_fir_taps - 1U);
        const double window = 0.42 - 0.5 * det::cos(phase) + 0.08 * det::cos(2.0 * phase);
        kernel[index] = ideal * window;
        energy += kernel[index] * kernel[index];
    }

    const double norm = 1.0 / std::sqrt(energy);

    for (std::size_t index = 0U; index < band_fir_taps; ++index)
    {
        kernel_storage[index] = static_cast<float>(kernel[index] * norm);
    }

    kernel_ready = true;
}

} // namespace

const float* band_fir_kernel() noexcept
{
    if (!kernel_ready)
    {
        build_kernel();
    }

    return kernel_storage;
}

void BandFir::reset(det::Pcg32* rng, bool stationary) noexcept
{
    (void)band_fir_kernel();

    for (std::size_t index = 0U; index < band_fir_history; ++index)
    {
        history_[index] = (stationary && rng != nullptr) ? static_cast<float>(rng->gaussian()) : 0.0f;
    }
}

void BandFir::filter(const float* input, float* output, std::size_t count, float* scratch) noexcept
{
    const float* kernel = band_fir_kernel();

    // scratch = history ++ input (the convolution reads only from scratch, so output may alias input).
    for (std::size_t index = 0U; index < band_fir_history; ++index)
    {
        scratch[index] = history_[index];
    }

    for (std::size_t index = 0U; index < count; ++index)
    {
        scratch[band_fir_history + index] = input[index];
    }

    // Symmetric kernel: pair the outer taps, fixed summation order for determinism.
    for (std::size_t n = 0U; n < count; ++n)
    {
        const float* window = scratch + n; // window[k] multiplies kernel[taps-1-k]
        float accumulator = 0.0f;

        for (std::size_t k = 0U; k < band_fir_half; ++k)
        {
            accumulator += kernel[k] * (window[band_fir_taps - 1U - k] + window[k]);
        }

        accumulator += kernel[band_fir_half] * window[band_fir_half];
        output[n] = accumulator;
    }

    // Retain the last band_fir_history samples of (history ++ input).
    const std::size_t total = band_fir_history + count;

    for (std::size_t index = 0U; index < band_fir_history; ++index)
    {
        history_[index] = scratch[total - band_fir_history + index];
    }
}

} // namespace signal_lab
