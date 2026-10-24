// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// Catch2 suite of the Hilbert FIR and the analytic ring (signal_lab/analytic_filter.hpp):
// design 3.1 to 3.4 and 4.2, tests U4 to U6.
// Guide: docs/modules/signal-lab-watterson.md, "Hilbert filter".

#include "signal_lab/analytic_filter.hpp"
#include "signal_lab/convert.hpp"
#include "signal_lab/det_math.hpp"
#include "signal_lab/kaiser.hpp"
#include "signal_lab/stream_rate.hpp"
#include "test_spectrum.hpp"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <numbers>
#include <span>
#include <vector>

namespace
{

namespace sl = signal_lab;
namespace units = signal_lab::units;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using signal_lab_test::Complex;
using signal_lab_test::two_pi;

constexpr double rate_hz = 48000.0;
constexpr double nyquist_hz = rate_hz / 2.0;
// |H| of an antisymmetric filter is twice the sine sum over one side.
constexpr double antisymmetric_gain = 2.0;
// A uniform draw in [0, 1) less this is centred on zero.
constexpr double uniform_centre = 0.5;
// U4: each coefficient against 2 / (pi m) w_K[m] evaluated in the C library
// (std::cyl_bessel_i): float storage holds it to half an ulp, 6e-8 relative.
constexpr double coefficient_bound = 1.0e-7;
// U5: |H| within 4.2e-4 of 1 over 300 Hz ... fs / 2 - 300 Hz (design 3.3 computes 4.1e-4).
constexpr double magnitude_bound = 4.2e-4;
constexpr double magnitude_low_hz = 300.0;
constexpr double magnitude_step_hz = 10.0;
// U6: a cosine's mirror at or below -70 dBc (lane criterion 4).
constexpr double mirror_limit_db = -70.0;
constexpr std::array mirror_tones_hz{300.0, 1000.0, 1800.0, 3300.0, 12000.0};
// One second at 48 kHz: every tone above is a whole number of cycles in it.
constexpr std::size_t measure_frames = 48000U;
// The positive-frequency component of xa = e^(j w n) has unit magnitude.
constexpr double positive_bound = 1.0e-3;

// Runs input through a filter in chunks and returns the analytic samples, frame by frame.
[[nodiscard]] std::vector<sl::AnalyticSample> analytic_of(std::span<const float> input)
{
    auto filter = std::make_unique<sl::AnalyticFilter>();
    auto ring = std::make_unique<sl::AnalyticRing>();
    filter->configure();
    ring->clear();
    std::vector<sl::AnalyticSample> output;
    output.reserve(input.size());
    const std::size_t chunk = sl::analytic_chunk_frames.value();

    for (std::size_t offset = 0U; offset < input.size(); offset += chunk)
    {
        const std::size_t count = std::min(chunk, input.size() - offset);
        const units::SampleIndex first{offset};
        filter->process(input.subspan(offset, count), first, *ring);

        for (std::size_t frame = 0U; frame < count; ++frame)
        {
            output.push_back(ring->read(first + units::SampleCount{frame}));
        }
    }

    return output;
}

} // namespace

TEST_CASE("analytic_filter: type III structure, D = 215, coefficients 2 / (pi m) w_K[m] (U4)", "[analytic_filter][watterson]")
{
    auto filter = std::make_unique<sl::AnalyticFilter>();
    filter->configure();
    const std::span<const float, sl::hilbert_pairs> coefficients = filter->coefficients();
    CHECK(sl::hilbert_taps == 2U * sl::hilbert_delay + 1U);
    CHECK(sl::hilbert_delay == 215U);
    CHECK(sl::hilbert_latency.value() == sl::hilbert_delay);
    const double beta = sl::kaiser::beta(sl::hilbert_attenuation);
    const auto delay = sl::as_double(sl::hilbert_delay);

    for (std::size_t pair = 0U; pair < sl::hilbert_pairs; ++pair)
    {
        const auto offset = sl::as_double(2U * pair + 1U);
        const double ratio = offset / delay;
        const double window = std::cyl_bessel_i(0.0, beta * std::sqrt(std::max(0.0, 1.0 - ratio * ratio))) / std::cyl_bessel_i(0.0, beta);
        const double expected = 2.0 / (std::numbers::pi * offset) * window;
        INFO("m = " << offset);
        CHECK_THAT(sl::as_double(coefficients[pair]), WithinRel(expected, coefficient_bound));
    }

    // The end taps are non-zero (4k + 3), so the full length is used.
    CHECK(coefficients[sl::hilbert_pairs - 1U] > 0.0F);
}

TEST_CASE("analytic_filter: |H| is within 4.2e-4 of 1 from 300 Hz to fs / 2 - 300 Hz (U5)", "[analytic_filter][watterson]")
{
    auto filter = std::make_unique<sl::AnalyticFilter>();
    filter->configure();
    const std::span<const float, sl::hilbert_pairs> coefficients = filter->coefficients();
    double worst = 0.0;

    for (double frequency = magnitude_low_hz; frequency <= nyquist_hz - magnitude_low_hz; frequency += magnitude_step_hz)
    {
        // H(w) = -2j sum over odd m of h[m] sin(w m) for an antisymmetric filter.
        double sum = 0.0;

        for (std::size_t pair = 0U; pair < sl::hilbert_pairs; ++pair)
        {
            const auto offset = sl::as_double(2U * pair + 1U);
            sum += sl::as_double(coefficients[pair]) * std::sin(two_pi * frequency * offset / rate_hz);
        }

        worst = std::max(worst, std::abs(antisymmetric_gain * sum - 1.0));
    }

    CHECK(worst <= magnitude_bound);
}

TEST_CASE("analytic_filter: a cosine keeps its positive frequency, the mirror is at most -70 dBc (U6)", "[analytic_filter][watterson]")
{
    const std::size_t settle = sl::hilbert_taps;

    for (const double tone : mirror_tones_hz)
    {
        std::vector<float> input(settle + measure_frames);

        for (std::size_t frame = 0U; frame < input.size(); ++frame)
        {
            input[frame] = sl::to_float(std::cos(two_pi * tone * sl::as_double(frame) / rate_hz));
        }

        const std::vector<sl::AnalyticSample> analytic = analytic_of(input);
        Complex positive{};
        Complex negative{};

        for (std::size_t frame = settle; frame < analytic.size(); ++frame)
        {
            const Complex value{analytic[frame].in_phase, analytic[frame].quadrature};
            const double angle = two_pi * tone * sl::as_double(frame) / rate_hz;
            positive += value * Complex{std::cos(angle), -std::sin(angle)};
            negative += value * Complex{std::cos(angle), std::sin(angle)};
        }

        const auto count = sl::as_double(measure_frames);
        INFO("tone " << tone << " Hz");
        CHECK_THAT(std::abs(positive) / count, WithinAbs(1.0, positive_bound));
        CHECK(signal_lab_test::amplitude_db(std::abs(negative) / std::abs(positive)) <= mirror_limit_db);
    }
}

TEST_CASE("analytic_filter: the in-phase sample is the input delayed by D frames, exactly", "[analytic_filter][watterson]")
{
    constexpr std::size_t frames = 4096U;
    constexpr std::uint64_t pattern_seed = 7U;
    std::vector<float> input(frames);
    signal_lab::det::Pcg32 generator{pattern_seed, 0U, 0U};

    for (float& sample : input)
    {
        sample = sl::to_float(generator.uniform() - uniform_centre);
    }

    const std::vector<sl::AnalyticSample> analytic = analytic_of(input);

    for (std::size_t frame = 0U; frame < frames; ++frame)
    {
        const float expected = frame >= sl::hilbert_delay ? input[frame - sl::hilbert_delay] : 0.0F;

        if (analytic[frame].in_phase != expected)
        {
            FAIL("frame " << frame << " in-phase differs from x(n - D)");
        }
    }
}
