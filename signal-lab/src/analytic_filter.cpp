// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// The Hilbert FIR and the analytic ring (signal_lab/analytic_filter.hpp).
// Guide: docs/modules/signal-lab-watterson.md, "Hilbert filter".
//
// Float order (part of the stage's bit-exact definition): for each frame the
// quadrature is accumulated from 0.0f over p = 0 ... 107 in that order, each term
// h[2p + 1] * (x(n - D - m) - x(n - D + m)) rounded in float, with no contraction
// (-ffp-contract=off, /fp:strict).

#include "signal_lab/analytic_filter.hpp"

#include <algorithm>
#include <numbers>

namespace signal_lab
{

void AnalyticFilter::configure() noexcept
{
    const double beta = kaiser::beta(hilbert_attenuation);
    const double centre = as_double(hilbert_delay);
    const std::span<float, hilbert_pairs> coefficients{mCoefficients};

    for (std::uint32_t pair = 0U; pair < hilbert_pairs; ++pair)
    {
        const double offset = as_double(2U * pair + 1U);
        const double ideal = 2.0 / (std::numbers::pi * offset);
        coefficients[pair] = to_float(ideal * kaiser::window(beta, offset / centre));
    }

    mBuffer.fill(0.0F);
}

void AnalyticFilter::process(std::span<const float> input, units::SampleIndex first, AnalyticRing& ring) noexcept
{
    std::array<AnalyticSample, analytic_chunk_frames.value()> output{};
    process(input, first, output);
    const std::size_t count = std::min({input.size(), output.size(), to_size(analytic_chunk_frames.value())});

    for (std::size_t frame = 0U; frame < count; ++frame)
    {
        ring.write(first + units::SampleCount{frame}, output[frame]);
    }
}

void AnalyticFilter::process(std::span<const float> input, units::SampleIndex /*first*/, std::span<AnalyticSample> output) noexcept
{
    const std::span<float> buffer{mBuffer};
    const std::span<const float, hilbert_pairs> coefficients{mCoefficients};
    const std::size_t count = std::min({input.size(), output.size(), to_size(analytic_chunk_frames.value())});

    if (count == 0U)
    {
        return;
    }

    std::ranges::copy(input.first(count), buffer.subspan(hilbert_history).begin());

    for (std::size_t frame = 0U; frame < count; ++frame)
    {
        const std::size_t centre = hilbert_delay + frame;
        float quadrature = 0.0F;

        for (std::size_t pair = 0U; pair < hilbert_pairs; ++pair)
        {
            const std::size_t offset = 2U * pair + 1U;
            quadrature += coefficients[pair] * (buffer[centre - offset] - buffer[centre + offset]);
        }

        output[frame] = AnalyticSample{buffer[centre], quadrature};
    }

    // Keep the last 2 D frames as the history of the next chunk.
    std::ranges::copy(buffer.subspan(count, hilbert_history), buffer.begin());
}

void AnalyticRing::clear() noexcept
{
    mSamples.fill(AnalyticSample{});
}

} // namespace signal_lab
