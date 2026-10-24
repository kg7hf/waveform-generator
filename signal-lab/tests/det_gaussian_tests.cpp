// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// Catch2 suite of Box-Muller on PCG32 (signal_lab/det_gaussian.hpp): design 5.1,
// test U34, and the excitation half of U31/U32 (Gaussian, white).
// Guide: docs/modules/signal-lab-watterson.md, "Tap processes".

#include "signal_lab/convert.hpp"
#include "signal_lab/det_gaussian.hpp"
#include "signal_lab/det_math.hpp"
#include "test_spectrum.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <vector>

namespace
{

namespace det = signal_lab::det;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

// The draws that pin the edges of the transform (design 5.1).
constexpr std::uint32_t largest_draw = 0xFFFFFFFFU; // u1 = (2^32 - 1 + 1) 2^-32 = 1: radius 0
constexpr std::uint32_t smallest_draw = 0U;         // u1 = 2^-32: the largest radius
// sqrt(-2 ln 2^-32) = sqrt(64 ln 2): the bound on |component|, 6.66 sigma.
constexpr double largest_radius = 6.6604368892615815;
constexpr double radius_bound = 1.0e-13; // det::log within 1e-14 relative, then sqrt
// Angle draws whose top 24 bits are 0, 1/4, 1/2 and 3/4 of a turn: the four axes.
constexpr std::uint32_t quarter_turn_draw = 0x40000000U;
constexpr std::uint32_t half_turn_draw = 0x80000000U;
constexpr std::uint32_t three_quarter_turn_draw = 0xC0000000U;

// Seeded statistics: 2^22 pairs; every tolerance is at least 5 standard errors of
// its estimate (design 10.1).
constexpr std::uint64_t statistics_seed = 20261008U;
constexpr std::uint32_t statistics_family = 9U;
constexpr std::size_t statistics_pairs = std::size_t{1} << 22U;
constexpr double standard_errors = 5.0;
// Var(x^2) = 2 for x ~ N(0, 1), so the variance estimate has s.e. sqrt(2 / N) (6.9e-4).
constexpr double variance_of_square = 2.0;
// |w|^2 ~ Exp(1) for w = (first + j second) / sqrt(2): its variance is 1.
constexpr double variance_of_exponential = 1.0;
constexpr double components_per_tap = 2.0;
// The sample excess kurtosis of N Gaussians has s.e. sqrt(24 / N) (0.0024 at 2^22).
constexpr double kurtosis_variance_scale = 24.0;
constexpr double gaussian_kurtosis = 3.0;
constexpr double kurtosis_standard_errors = 4.0;
// Two-sided Gaussian tails: P(|x| > 3) = 2.70e-3, P(|x| > 4) = 6.33e-5 (erfc(k / sqrt 2)),
// within the binomial 99.9 % limits (z = 3.29).
constexpr double tail_three = 3.0;
constexpr double tail_four = 4.0;
constexpr double tail_z = 3.2905;

// E.5.3: white within +/-1 dB to fs / 2. Periodogram of 2^22 samples in 4096-point
// segments, averaged over 64-bin bands: each band holds 64 x 1024 chi-square(2) values,
// so its relative s.e. is 1 / 256 (0.017 dB) and 1 dB is far outside chance.
constexpr std::size_t whiteness_segment = 4096U;
constexpr std::size_t whiteness_band = 64U;
constexpr double whiteness_bound_db = 1.0;
// The excitation drawn as AWGN uses it: first then second as consecutive real samples.
constexpr std::size_t samples_per_pair = 2U;

struct Moments
{
    double variance{};
    double excess_kurtosis{};
    double beyond_three{};
    double beyond_four{};
};

[[nodiscard]] Moments moments_of(const std::vector<double>& values)
{
    double second = 0.0;
    double fourth = 0.0;
    double three = 0.0;
    double four = 0.0;

    for (const double value : values)
    {
        const double square = value * value;
        second += square;
        fourth += square * square;
        three += std::abs(value) > tail_three ? 1.0 : 0.0;
        four += std::abs(value) > tail_four ? 1.0 : 0.0;
    }

    const auto count = signal_lab::as_double(values.size());
    const double variance = second / count;
    return Moments{variance, fourth / count / (variance * variance) - gaussian_kurtosis, three / count, four / count};
}

[[nodiscard]] double two_sided_tail(double threshold)
{
    return std::erfc(threshold / std::numbers::sqrt2);
}

[[nodiscard]] bool tail_within_limits(double observed, double threshold, double count)
{
    const double expected = two_sided_tail(threshold);
    const double limit = tail_z * std::sqrt(expected * (1.0 - expected) / count);
    return std::abs(observed - expected) <= limit;
}

[[nodiscard]] std::vector<double> excitation(std::size_t pairs)
{
    det::Pcg32 generator{statistics_seed, statistics_family, 0U};
    std::vector<double> values;
    values.reserve(samples_per_pair * pairs);

    for (std::size_t index = 0U; index < pairs; ++index)
    {
        const det::GaussianPair pair = det::draw_gaussian_pair(generator);
        values.push_back(pair.first);
        values.push_back(pair.second);
    }

    return values;
}

} // namespace

TEST_CASE("det_gaussian: the largest radius draw is widened before the + 1 (U34)", "[det_gaussian]")
{
    const det::GaussianPair zero = det::gaussian_pair(largest_draw, 0U);
    CHECK(zero.first == 0.0);
    CHECK(zero.second == 0.0);
    const det::GaussianPair widest = det::gaussian_pair(smallest_draw, 0U);
    CHECK(std::isfinite(widest.first));
    CHECK_THAT(widest.first, WithinRel(largest_radius, radius_bound));
    CHECK(widest.second == 0.0);
}

TEST_CASE("det_gaussian: angle draws of 0, 1/4, 1/2 and 3/4 turn give the four axes (U34)", "[det_gaussian]")
{
    const double radius = det::gaussian_pair(smallest_draw, 0U).first;
    const det::GaussianPair quarter = det::gaussian_pair(smallest_draw, quarter_turn_draw);
    const det::GaussianPair half = det::gaussian_pair(smallest_draw, half_turn_draw);
    const det::GaussianPair three_quarter = det::gaussian_pair(smallest_draw, three_quarter_turn_draw);
    CHECK(quarter.first == 0.0);
    CHECK(quarter.second == radius);
    CHECK(half.first == -radius);
    CHECK(half.second == 0.0);
    CHECK(three_quarter.first == 0.0);
    CHECK(three_quarter.second == -radius);
}

TEST_CASE("det_gaussian: a pair takes exactly two draws, radius first (U34)", "[det_gaussian]")
{
    det::Pcg32 drawn{statistics_seed, statistics_family, 1U};
    det::Pcg32 manual{statistics_seed, statistics_family, 1U};
    const det::GaussianPair pair = det::draw_gaussian_pair(drawn);
    const std::uint32_t radius_draw = manual.next();
    const std::uint32_t angle_draw = manual.next();
    const det::GaussianPair expected = det::gaussian_pair(radius_draw, angle_draw);
    CHECK(pair.first == expected.first);
    CHECK(pair.second == expected.second);
    CHECK(drawn.next() == manual.next());
}

TEST_CASE("det_gaussian: each component has unit variance and the tap form unit power (U34)", "[det_gaussian][statistics]")
{
    det::Pcg32 generator{statistics_seed, statistics_family, 0U};
    double first = 0.0;
    double second = 0.0;
    double tap_power = 0.0;

    for (std::size_t index = 0U; index < statistics_pairs; ++index)
    {
        const det::GaussianPair pair = det::draw_gaussian_pair(generator);
        first += pair.first * pair.first;
        second += pair.second * pair.second;
        tap_power += (pair.first * pair.first + pair.second * pair.second) / components_per_tap;
    }

    const auto count = signal_lab::as_double(statistics_pairs);
    const double variance_bound = standard_errors * std::sqrt(variance_of_square / count);
    CHECK_THAT(first / count, WithinAbs(1.0, variance_bound));
    CHECK_THAT(second / count, WithinAbs(1.0, variance_bound));
    CHECK_THAT(tap_power / count, WithinAbs(1.0, standard_errors * std::sqrt(variance_of_exponential / count)));
}

TEST_CASE("det_gaussian: the excitation is Gaussian in kurtosis and tails (U31)", "[det_gaussian][statistics]")
{
    const std::vector<double> values = excitation(statistics_pairs);
    const Moments moments = moments_of(values);
    const auto count = signal_lab::as_double(values.size());
    CHECK_THAT(moments.excess_kurtosis, WithinAbs(0.0, kurtosis_standard_errors * std::sqrt(kurtosis_variance_scale / count)));
    CHECK(tail_within_limits(moments.beyond_three, tail_three, count));
    CHECK(tail_within_limits(moments.beyond_four, tail_four, count));
}

TEST_CASE("det_gaussian: the excitation is white within 1 dB to fs / 2 (U32, E.5.3)", "[det_gaussian][statistics]")
{
    const std::vector<double> values = excitation(statistics_pairs / samples_per_pair);
    std::vector<signal_lab_test::Complex> sequence(values.begin(), values.end());
    const std::vector<double> power = signal_lab_test::average_periodogram(sequence, whiteness_segment);
    // A real sequence of unit variance has a mean periodogram of N per bin.
    const auto expected = signal_lab::as_double(whiteness_segment);

    for (std::size_t start = 0U; start < whiteness_segment / 2U; start += whiteness_band)
    {
        double band = 0.0;

        for (std::size_t bin = start; bin < start + whiteness_band; ++bin)
        {
            band += power[bin];
        }

        INFO("bins from " << start);
        CHECK(std::abs(signal_lab_test::power_db(band / signal_lab::as_double(whiteness_band) / expected)) <= whiteness_bound_db);
    }
}
