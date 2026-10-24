// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

#pragma once

// Analysis helpers for the Catch2 suites of the Watterson and noise stages:
// a radix-2 FFT, single-frequency correlations and a least-squares line. Host
// tests only, in double precision with the C library: they measure the stages,
// they are not part of them, so they may allocate.

#include "signal_lab/convert.hpp"

#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <span>
#include <utility>
#include <vector>

namespace signal_lab_test
{

using Complex = std::complex<double>;

inline constexpr double two_pi = 2.0 * std::numbers::pi;
// 10 log10 and 20 log10: the decibel for powers and for amplitudes.
inline constexpr double power_db_per_decade = 10.0;
inline constexpr double amplitude_db_per_decade = 20.0;
// The Hann window, w = (1 - cos(2 pi n / N)) / 2.
inline constexpr double hann_scale = 0.5;

[[nodiscard]] inline double power_db(double ratio)
{
    return power_db_per_decade * std::log10(ratio);
}

[[nodiscard]] inline double amplitude_db(double ratio)
{
    return amplitude_db_per_decade * std::log10(ratio);
}

// In-place iterative radix-2 FFT, X[k] = sum_n x[n] exp(-j 2 pi k n / N); N a power of
// two. The twiddles are computed once per size, directly (not by recurrence).
class Fft
{
public:
    explicit Fft(std::size_t count) : mTwiddles(count / 2U)
    {
        for (std::size_t index = 0U; index < mTwiddles.size(); ++index)
        {
            const double angle = -two_pi * signal_lab::as_double(index) / signal_lab::as_double(count);
            mTwiddles[index] = Complex{std::cos(angle), std::sin(angle)};
        }
    }

    void transform(std::span<Complex> data) const
    {
        const std::size_t count = data.size();

        for (std::size_t index = 1U, reversed = 0U; index < count; ++index)
        {
            std::size_t bit = count >> 1U;

            for (; (reversed & bit) != 0U; bit >>= 1U)
            {
                reversed ^= bit;
            }

            reversed ^= bit;

            if (index < reversed)
            {
                std::swap(data[index], data[reversed]);
            }
        }

        for (std::size_t length = 2U; length <= count; length <<= 1U)
        {
            const std::size_t half = length / 2U;
            const std::size_t stride = count / length;

            for (std::size_t start = 0U; start < count; start += length)
            {
                for (std::size_t offset = 0U; offset < half; ++offset)
                {
                    const Complex even = data[start + offset];
                    const Complex odd = data[start + offset + half] * mTwiddles[offset * stride];
                    data[start + offset] = even + odd;
                    data[start + offset + half] = even - odd;
                }
            }
        }
    }

private:
    std::vector<Complex> mTwiddles;
};

enum class Window : std::uint8_t
{
    rectangular,
    hann,
};

// The average periodogram |X[k]|^2 of consecutive, non-overlapping segments of a
// complex sequence (Welch), rectangular or Hann windowed. Bin k is the frequency
// k / segment cycles per sample; the result is not normalised.
[[nodiscard]] inline std::vector<double> average_periodogram(std::span<const Complex> sequence, std::size_t segment, Window window = Window::rectangular)
{
    std::vector<double> power(segment, 0.0);
    std::vector<Complex> work(segment);
    std::vector<double> taper(segment, 1.0);
    const Fft fft{segment};
    const std::size_t segments = sequence.size() / segment;

    if (window == Window::hann)
    {
        for (std::size_t index = 0U; index < segment; ++index)
        {
            const double cosine = std::cos(two_pi * signal_lab::as_double(index) / signal_lab::as_double(segment));
            taper[index] = hann_scale * (1.0 - cosine);
        }
    }

    for (std::size_t index = 0U; index < segments; ++index)
    {
        const auto part = sequence.subspan(index * segment, segment);

        for (std::size_t sample = 0U; sample < segment; ++sample)
        {
            work[sample] = part[sample] * taper[sample];
        }

        fft.transform(work);

        for (std::size_t bin = 0U; bin < segment; ++bin)
        {
            power[bin] += std::norm(work[bin]);
        }
    }

    for (double& value : power)
    {
        value /= signal_lab::as_double(segments);
    }

    return power;
}

// The signed bin offset of bin k in an N-point spectrum: k for k < N / 2, k - N above.
[[nodiscard]] inline double signed_bin(std::size_t bin, std::size_t count)
{
    return bin < count / 2U ? signal_lab::as_double(bin) : signal_lab::as_double(bin) - signal_lab::as_double(count);
}

// The correlation (1 / N) sum x[n] exp(-j 2 pi f n / fs) of a real sequence: the
// complex amplitude of a component at f, half its peak for a real cosine.
[[nodiscard]] inline Complex correlate(std::span<const float> samples, double frequency, double rate)
{
    Complex sum{};

    for (std::size_t index = 0U; index < samples.size(); ++index)
    {
        const double angle = -two_pi * frequency * signal_lab::as_double(index) / rate;
        sum += signal_lab::as_double(samples[index]) * Complex{std::cos(angle), std::sin(angle)};
    }

    return sum / signal_lab::as_double(samples.size());
}

// The least-squares line y = intercept + slope x.
struct Line
{
    double intercept{};
    double slope{};
};

[[nodiscard]] inline Line fit_line(std::span<const double> x, std::span<const double> y)
{
    double sx = 0.0;
    double sy = 0.0;
    double sxx = 0.0;
    double sxy = 0.0;
    const auto count = signal_lab::as_double(x.size());

    for (std::size_t index = 0U; index < x.size(); ++index)
    {
        sx += x[index];
        sy += y[index];
        sxx += x[index] * x[index];
        sxy += x[index] * y[index];
    }

    const double slope = (count * sxy - sx * sy) / (count * sxx - sx * sx);
    return Line{(sy - slope * sx) / count, slope};
}

} // namespace signal_lab_test
