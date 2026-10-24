// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// The E.5.4 Gaussian Doppler FIR and the node process (signal_lab/doppler_filter.hpp).
// Guide: docs/modules/signal-lab-watterson.md, "Tap processes".
//
// Float order (part of the stage's bit-exact definition): a node is accumulated
// from 0.0f, real and imaginary parts separately, over the symmetric pairs from the
// outermost (k = 0) inwards, each term c_k * (w_old + w_new), then the centre tap,
// then scaled by the path's float amplitude.

#include "signal_lab/doppler_filter.hpp"

#include "signal_lab/det_gaussian.hpp"

#include <algorithm>
#include <cmath>

namespace signal_lab
{
namespace
{

// E|w|^2 = 1 for the tap excitation: each component of a unit-variance pair times 1 / sqrt(2).
constexpr double inverse_sqrt2 = std::numbers::sqrt2 / 2.0;
// Each tap off the centre appears twice in the symmetric FIR.
constexpr double symmetric_pair_weight = 2.0;
constexpr const char* design_message = "design_doppler: the spread must be finite and in [0.1, 10] Hz, at a rate of at least 64 Hz, within the static FIR capacity";

[[nodiscard]] m110::Status refused() noexcept
{
    return m110::Status{m110::StatusCode::invalid_argument, design_message};
}

// The prose form of E.5.4 at offset k from the centre: exp(-(pi k / K)^2).
[[nodiscard]] double gaussian_tap(std::uint32_t offset, double rate_multiple) noexcept
{
    const double argument = std::numbers::pi * as_double(offset) / rate_multiple;
    return det::exp(-(argument * argument));
}

} // namespace

m110::Result<DopplerDesign> design_doppler(units::FrequencyDelta spread, units::SampleRate rate) noexcept
{
    const double hertz = spread.in(units::Hz);

    // The range bounds the quotients below: 750 / d <= 7500 frames, K < 64.86 at 48 kHz.
    if (!std::isfinite(hertz) || hertz < watterson_min_spread.in(units::Hz) || hertz > watterson_max_spread.in(units::Hz) || rate.hertz() < tap_rate_multiple_min)
    {
        return refused();
    }

    const units::FrequencyDelta scale{as_double(rate.hertz()) / as_double(tap_rate_multiple_min), units::Hz};
    const std::uint32_t frames = std::max(floor_to_uint32(scale / spread), std::uint32_t{1});
    const units::FrequencyDelta node_rate{as_double(rate.hertz()) / as_double(frames), units::Hz};
    const double multiple = node_rate / spread;
    // L = ceil(tau R) = ceil(K sqrt(ln(1 / f)) / pi); at the stream rate every spread in range fits.
    const std::uint32_t half = ceil_to_uint32(multiple * std::sqrt(doppler_truncation_log) / std::numbers::pi);

    if (half > doppler_max_half)
    {
        return refused();
    }

    return DopplerDesign{units::SampleCount{frames}, node_rate, multiple, half, 2U * half + 1U};
}

double interpolation_power(double lag_one_correlation, units::SampleCount frames_per_node) noexcept
{
    constexpr double interval_average_scale = 3.0; // the mean of mu (1 - mu) over the interval is (M^2 - 1) / (6 M^2)
    const double frames = as_double(frames_per_node.value());
    const double square = frames * frames;
    return 1.0 - (1.0 - lag_one_correlation) * (square - 1.0) / (interval_average_scale * square);
}

void DopplerFilter::clear() noexcept
{
    mCoefficients.fill(0.0F);
    mHistory.fill(ComplexFloat{});
    mGenerator = det::Pcg32{};
    mHalf = 0U;
    mTaps = 0U;
    mOldest = 0U;
    mAmplitude = 0.0F;
}

void DopplerFilter::design(const DopplerDesign& design) noexcept
{
    mHalf = design.half;
    mTaps = design.taps;
    const std::span<float> coefficients{mCoefficients};
    double energy = 0.0;

    for (std::uint32_t offset = 0U; offset <= mHalf; ++offset)
    {
        const double tap = gaussian_tap(offset, design.rate_multiple);
        energy += (offset == 0U ? 1.0 : symmetric_pair_weight) * tap * tap;
    }

    const double scale = 1.0 / std::sqrt(energy);
    std::ranges::fill(coefficients, 0.0F);

    for (std::uint32_t index = 0U; index <= mHalf; ++index)
    {
        coefficients[index] = to_float(gaussian_tap(mHalf - index, design.rate_multiple) * scale);
    }
}

double DopplerFilter::energy() const noexcept
{
    const std::span<const float> coefficients = half_coefficients();
    double sum = 0.0;

    for (std::uint32_t index = 0U; index < mTaps; ++index)
    {
        const double tap = as_double(coefficients[std::min(index, mTaps - 1U - index)]);
        sum += tap * tap;
    }

    return sum;
}

double DopplerFilter::lag_one_correlation() const noexcept
{
    const std::span<const float> coefficients = half_coefficients();
    double lagged = 0.0;

    for (std::uint32_t index = 0U; index + 1U < mTaps; ++index)
    {
        const double current = as_double(coefficients[std::min(index, mTaps - 1U - index)]);
        const double next = as_double(coefficients[std::min(index + 1U, mTaps - 2U - index)]);
        lagged += current * next;
    }

    return lagged / energy();
}

ComplexFloat DopplerFilter::prime(const det::Pcg32& generator, float amplitude) noexcept
{
    mGenerator = generator;
    mAmplitude = amplitude;
    mOldest = 0U;
    mHistory.fill(ComplexFloat{});

    for (std::uint32_t index = 0U; index < mTaps; ++index)
    {
        push(draw());
    }

    return mAmplitude * filter();
}

ComplexFloat DopplerFilter::next_node() noexcept
{
    push(draw());
    return mAmplitude * filter();
}

ComplexFloat DopplerFilter::draw() noexcept
{
    const det::GaussianPair pair = det::draw_gaussian_pair(mGenerator);
    return ComplexFloat{to_float(pair.first * inverse_sqrt2), to_float(pair.second * inverse_sqrt2)};
}

void DopplerFilter::push(ComplexFloat draw) noexcept
{
    const std::span<ComplexFloat> history{mHistory};
    history[mOldest] = draw;
    history[mOldest + mTaps] = draw;
    mOldest = mOldest + 1U == mTaps ? 0U : mOldest + 1U;
}

ComplexFloat DopplerFilter::filter() const noexcept
{
    // The last N draws, oldest first, as one contiguous view of the mirrored history.
    const std::span<const ComplexFloat> window = std::span{mHistory}.subspan(mOldest, mTaps);
    const std::span<const float> coefficients = half_coefficients();
    float real = 0.0F;
    float imag = 0.0F;

    for (std::uint32_t index = 0U; index < mHalf; ++index)
    {
        const ComplexFloat older = window[index];
        const ComplexFloat newer = window[mTaps - 1U - index];
        real += coefficients[index] * (older.real + newer.real);
        imag += coefficients[index] * (older.imag + newer.imag);
    }

    const ComplexFloat centre = window[mHalf];
    real += coefficients[mHalf] * centre.real;
    imag += coefficients[mHalf] * centre.imag;
    return ComplexFloat{real, imag};
}

} // namespace signal_lab
