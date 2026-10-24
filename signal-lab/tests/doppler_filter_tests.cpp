// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// Catch2 suite of the Watterson tap process (signal_lab/doppler_filter.hpp):
// design 5 and 6, tests U10 to U14, U16, U17 and U18 (a).
// Guide: docs/modules/signal-lab-watterson.md, "Tap processes".
//
// The design checks (U10 to U13 a) evaluate the stored float coefficients'
// spectrum in double with the C library; the statistical checks (U14, U16, U17)
// draw seeded node sequences, so their results are deterministic, and each
// tolerance is at least 5 standard errors of its estimate (design 10.1).

#include "signal_lab/convert.hpp"
#include "signal_lab/det_math.hpp"
#include "signal_lab/doppler_filter.hpp"
#include "signal_lab/stream_rate.hpp"
#include "signal_lab/watterson.hpp"
#include "test_spectrum.hpp"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
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

constexpr double one_hertz = 1.0;
constexpr std::array design_spreads_hz{0.1, 0.5, 1.0, 2.0, 5.0, 10.0};
constexpr std::array statistics_spreads_hz{0.1, 1.0, 10.0};

// U10: the lobe-fit two sigma within 0.2 % of d (design 5.2 computes 1.0006 d).
constexpr double lobe_fit_bound = 0.002;
// The fit uses the top 30 dB of the lobe within +/-3 d (design 5.2).
constexpr double lobe_fit_depth_db = -30.0;
constexpr double lobe_fit_extent = 3.0; // in units of d
constexpr double spectrum_step = 0.002; // in units of d: sigma = 0.5 d spans 250 steps
// The QA #7/#8 trap: the printed formula read as exp(-pi^2 d^2 t^2 / 2), which halves
// the exponent; truncated at the same 1 % it gives sigma = 0.354 d.
constexpr double trap_exponent_scale = 0.5;
constexpr double trap_two_sigma = 0.7071; // 2 x 0.354
constexpr double trap_bound = 0.01;

// U11: E.5.4's 1 % of peak; 89 taps at 1 Hz (K = 64), 89 or 91 over the range.
constexpr double truncation_fraction = 0.01;
constexpr std::uint32_t taps_at_one_hertz = 89U;
constexpr std::uint32_t taps_at_largest_multiple = 91U;

// U12: d over [0.1, 10] Hz in steps of 0.01 Hz, and the edges of the M = 75 interval.
constexpr double grid_step_hz = 0.01;
constexpr std::uint32_t grid_points = 991U;
constexpr double rate_multiple_ulp = 64.0 * 0x1p-52;
// R = fs / M is one IEEE quotient, so R M is fs to within an ulp or two.
constexpr double node_rate_bound = 1.0e-15;

// U13 (a): images at +/-R at most -79 dBc, the largest truncation sidelobe (+/-0.25 d)
// at most -63.5 dBc, and Table E-I's -20 and -30 dB points within 0.5 dB of the ideal
// (E-I allows 1.5 and 2.0 dB). Design 5.4 computes -79.6, -64.0, -0.09 and +0.34.
constexpr double image_limit_db = -79.0;
constexpr double sidelobe_limit_db = -63.5;
constexpr double sidelobe_halfwidth = 0.25; // in units of d
constexpr double table_e1_bound_db = 0.5;
constexpr double lobe_extent = 2.0;    // +/-4 sigma = +/-2 d
constexpr double sidelobe_start = 3.0; // beyond 3 d (6 sigma)
// The ideal Gaussian with sigma = d / 2 is 20 dB down at d sqrt(2 ln 100) / 2, 30 dB at d sqrt(2 ln 1000) / 2.
constexpr double twenty_db = 20.0;
constexpr double thirty_db = 30.0;
constexpr double hundred = 100.0;
constexpr double thousand = 1000.0;

// Statistics: one seed, the Watterson family, path streams 0 and 1 of stage 0.
constexpr std::uint64_t statistics_seed = 1U;
constexpr std::size_t rayleigh_nodes = std::size_t{1} << 22U;
constexpr std::size_t spectrum_nodes = std::size_t{1} << 20U;
constexpr std::size_t node_segment = 2048U;
// The intensity correlation of a Gaussian-PSD process with sigma_u = 1 / (2 K) of the
// node rate integrates to 1 / (2 sqrt(pi) sigma_u) = K / sqrt(pi) nodes (36.1 at K = 64).
constexpr double node_correlation_scale = std::numbers::inv_sqrtpi;
// U14 tolerances (design 10.1): E|h|^2 = 1 +/- 2 %, E|h|^4 / E^2 = 2 +/- 0.07,
// mean / RMS = sqrt(pi) / 2 = 0.8862 +/- 0.008, KS below its 99 % critical value.
constexpr double power_bound = 0.02;
constexpr double rayleigh_fourth_moment = 2.0;
constexpr double fourth_moment_bound = 0.07;
constexpr double rayleigh_mean_over_rms = 1.0 / (2.0 * std::numbers::inv_sqrtpi); // sqrt(pi) / 2
constexpr double mean_over_rms_bound = 0.008;
constexpr double ks_critical_99 = 1.628;
// U16: |rho_ij| <= 4 / sqrt(N_eff).
constexpr double correlation_standard_errors = 4.0;
// U17: the second moment over +/-4 sigma gives two sigma within 3 % of d.
constexpr double spread_bound = 0.03;
constexpr double expected_sigma_per_spread = sl::doppler_sigma_per_spread;
constexpr double moment_extent_sigmas = 4.0;
constexpr double sigmas_per_two_sigma = 2.0;
// A Gaussian lobe: ln S = c0 - fd^2 / (2 sigma^2), so sigma^2 = -1 / (2 c1).
constexpr double gaussian_log_scale = 2.0;
// Below this |u| the interpolator's response is 1 (its limit at u = 0).
constexpr double droop_origin = 1.0e-12;
// The cross term of the interpolation weights: 2 mu (1 - mu) rho1.
constexpr double cross_term_weight = 2.0;

// The design's refusals: outside [0.1, 10] Hz, not finite, or a rate below 64 Hz.
constexpr std::array refused_spreads_hz{0.0, 0.05, 10.5, -1.0, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()};
constexpr std::uint32_t too_slow_rate_hz = 32U;

// U18 (a): the closed form of v_i against the brute-force mean over j, to 1e-12.
constexpr std::array frames_per_node_cases{75U, 750U, 7500U};
constexpr double interpolation_bound = 1.0e-12;

[[nodiscard]] sl::DopplerDesign design_at(double spread_hz)
{
    return sl::design_doppler(units::FrequencyDelta{spread_hz, units::Hz}, sl::stream_rate).value();
}

// The full symmetric FIR as doubles, from the stored half coefficients.
[[nodiscard]] std::vector<double> full_taps(const sl::DopplerFilter& filter)
{
    const std::span<const float> half = filter.half_coefficients();
    const std::uint32_t taps = filter.taps();
    std::vector<double> full(taps);

    for (std::uint32_t index = 0U; index < taps; ++index)
    {
        full[index] = sl::as_double(half[std::min(index, taps - 1U - index)]);
    }

    return full;
}

// The 48 kHz PSD of the interpolated tap process at fd (in units of d): |C(u)|^2 times
// the linear interpolator's response (sin(pi u) / (M sin(pi u / M)))^4, u = fd / K.
[[nodiscard]] double tap_psd(std::span<const double> taps, double rate_multiple, std::uint64_t frames_per_node, double fd)
{
    const double u = fd / rate_multiple;
    const auto centre = sl::as_double(taps.size() - 1U) / 2.0;
    Complex response{};

    for (std::size_t index = 0U; index < taps.size(); ++index)
    {
        const double angle = -two_pi * u * (sl::as_double(index) - centre);
        response += taps[index] * Complex{std::cos(angle), std::sin(angle)};
    }

    const auto frames = sl::as_double(frames_per_node);
    const double droop = std::abs(u) < droop_origin ? 1.0 : std::sin(std::numbers::pi * u) / (frames * std::sin(std::numbers::pi * u / frames));
    return std::norm(response) * droop * droop * droop * droop;
}

// Two sigma, in units of d, from a least-squares fit of ln S = c0 + c1 fd^2 over the
// part of the lobe within the fit depth (design 5.2's "lobe fit").
[[nodiscard]] double lobe_fit_two_sigma(std::span<const double> taps, double rate_multiple, std::uint64_t frames_per_node)
{
    const double peak = tap_psd(taps, rate_multiple, frames_per_node, 0.0);
    std::vector<double> squares;
    std::vector<double> logs;

    for (double fd = 0.0; fd < lobe_fit_extent; fd += spectrum_step)
    {
        const double level = tap_psd(taps, rate_multiple, frames_per_node, fd) / peak;

        if (signal_lab_test::power_db(level) >= lobe_fit_depth_db)
        {
            squares.push_back(fd * fd);
            logs.push_back(std::log(level));
        }
    }

    const signal_lab_test::Line line = signal_lab_test::fit_line(squares, logs);
    return sigmas_per_two_sigma * std::sqrt(-1.0 / (gaussian_log_scale * line.slope));
}

// A unit-energy Gaussian FIR exp(-scale (pi k / K)^2), truncated where it falls to 1 %.
[[nodiscard]] std::vector<double> gaussian_taps(double rate_multiple, double exponent_scale)
{
    const std::uint32_t half = sl::ceil_to_uint32(rate_multiple * std::sqrt(std::log(1.0 / truncation_fraction) / exponent_scale) / std::numbers::pi);
    const std::int64_t extent{half};
    std::vector<double> taps;
    double energy = 0.0;

    for (std::int64_t offset = -extent; offset <= extent; ++offset)
    {
        const double argument = std::numbers::pi * sl::as_double(offset) / rate_multiple;
        taps.push_back(std::exp(-exponent_scale * argument * argument));
        energy += taps.back() * taps.back();
    }

    for (double& tap : taps)
    {
        tap /= std::sqrt(energy);
    }

    return taps;
}

[[nodiscard]] std::unique_ptr<sl::DopplerFilter> primed_filter(double spread_hz, std::uint32_t stream, sl::ComplexFloat& first)
{
    auto filter = std::make_unique<sl::DopplerFilter>();
    filter->clear();
    filter->design(design_at(spread_hz));
    const auto amplitude = sl::to_float(1.0 / std::sqrt(filter->energy()));
    first = filter->prime(sl::det::Pcg32{statistics_seed, sl::watterson_rng_family, stream}, amplitude);
    return filter;
}

[[nodiscard]] std::vector<Complex> node_sequence(double spread_hz, std::uint32_t stream, std::size_t count)
{
    sl::ComplexFloat first{};
    auto filter = primed_filter(spread_hz, stream, first);
    std::vector<Complex> nodes;
    nodes.reserve(count);
    nodes.emplace_back(first.real, first.imag);

    while (nodes.size() < count)
    {
        const sl::ComplexFloat node = filter->next_node();
        nodes.emplace_back(node.real, node.imag);
    }

    return nodes;
}

[[nodiscard]] double effective_nodes(std::size_t count, double rate_multiple)
{
    return sl::as_double(count) / (rate_multiple * node_correlation_scale);
}

} // namespace

TEST_CASE("doppler_filter: sigma is d / 2, and the printed-formula trap fails the same check (U10)", "[doppler_filter][watterson]")
{
    for (const double spread : design_spreads_hz)
    {
        auto filter = std::make_unique<sl::DopplerFilter>();
        filter->clear();
        const sl::DopplerDesign design = design_at(spread);
        filter->design(design);
        INFO("d = " << spread << " Hz");
        CHECK_THAT(lobe_fit_two_sigma(full_taps(*filter), design.rate_multiple, design.frames_per_node.value()), WithinAbs(1.0, lobe_fit_bound));
    }

    const sl::DopplerDesign design = design_at(one_hertz);
    const std::vector<double> trap = gaussian_taps(design.rate_multiple, trap_exponent_scale);
    const double trap_spread = lobe_fit_two_sigma(trap, design.rate_multiple, design.frames_per_node.value());
    CHECK(std::abs(trap_spread - 1.0) > lobe_fit_bound);
    CHECK_THAT(trap_spread, WithinAbs(trap_two_sigma, trap_bound));
}

TEST_CASE("doppler_filter: 1 % truncation, 89 taps at 1 Hz, 89 or 91 over the range (U11)", "[doppler_filter][watterson]")
{
    for (const double spread : design_spreads_hz)
    {
        auto filter = std::make_unique<sl::DopplerFilter>();
        filter->clear();
        filter->design(design_at(spread));
        const std::span<const float> half = filter->half_coefficients();
        const float peak = half.back();
        INFO("d = " << spread << " Hz");
        CHECK(half.front() <= truncation_fraction * peak);
        CHECK(half[1] > truncation_fraction * peak);
        CHECK((filter->taps() == taps_at_one_hertz || filter->taps() == taps_at_largest_multiple));
    }

    CHECK(design_at(one_hertz).taps == taps_at_one_hertz);
    CHECK(sl::doppler_max_taps == taps_at_largest_multiple);
}

TEST_CASE("doppler_filter: a spread or rate outside the design's domain is refused as a value", "[doppler_filter][watterson]")
{
    for (const double spread : refused_spreads_hz)
    {
        INFO("d = " << spread << " Hz");
        const m110::Result<sl::DopplerDesign> design = sl::design_doppler(units::FrequencyDelta{spread, units::Hz}, sl::stream_rate);
        CHECK_FALSE(design.has_value());
        CHECK(design.status().code == m110::StatusCode::invalid_argument);
    }

    CHECK_FALSE(sl::design_doppler(units::FrequencyDelta{one_hertz, units::Hz}, units::SampleRate{too_slow_rate_hz}).has_value());
    CHECK(sl::design_doppler(sl::watterson_min_spread, sl::stream_rate).has_value());
    CHECK(sl::design_doppler(sl::watterson_max_spread, sl::stream_rate).has_value());
}

TEST_CASE("doppler_filter: the node rate is at least 64 d with whole frames per node (U12)", "[doppler_filter][watterson]")
{
    std::vector<double> spreads;
    spreads.reserve(grid_points + 2U);

    for (std::uint32_t point = 0U; point < grid_points; ++point)
    {
        spreads.push_back(sl::watterson_min_spread.in(units::Hz) + grid_step_hz * sl::as_double(point));
    }

    // The edges of the M = 75 interval: K is largest just above 750 / 76 Hz.
    constexpr double largest_multiple_spread = 750.0 / 76.0;
    spreads.push_back(std::nextafter(largest_multiple_spread, std::numeric_limits<double>::infinity()));
    spreads.push_back(sl::watterson_max_spread.in(units::Hz));

    for (const double spread : spreads)
    {
        const sl::DopplerDesign design = design_at(spread);
        const auto frames = sl::as_double(design.frames_per_node.value());
        INFO("d = " << spread << " Hz");
        CHECK(design.frames_per_node.value() >= sl::doppler_min_frames_per_node);
        const double node_rate = design.node_rate.in(units::Hz);
        CHECK_THAT(node_rate * frames, WithinRel(sl::as_double(sl::stream_rate.hertz()), node_rate_bound));
        CHECK(design.rate_multiple >= sl::as_double(sl::tap_rate_multiple_min) - rate_multiple_ulp);
        CHECK(design.rate_multiple < sl::doppler_rate_multiple_bound);
        CHECK(design.half <= sl::doppler_max_half);
    }
}

TEST_CASE("doppler_filter: images, sidelobes and Table E-I points of the designed process (U13 a)", "[doppler_filter][watterson]")
{
    auto filter = std::make_unique<sl::DopplerFilter>();
    filter->clear();
    const sl::DopplerDesign design = design_at(one_hertz);
    filter->design(design);
    const std::vector<double> taps = full_taps(*filter);
    const std::uint64_t frames = design.frames_per_node.value();
    const double multiple = design.rate_multiple;
    double lobe = 0.0;
    double image = 0.0;

    for (double fd = -lobe_extent; fd <= lobe_extent; fd += spectrum_step)
    {
        lobe += tap_psd(taps, multiple, frames, fd);
        image += tap_psd(taps, multiple, frames, fd + multiple); // the lobe's image at +R
    }

    CHECK(signal_lab_test::power_db(image / lobe) <= image_limit_db);
    // The largest truncation sidelobe beyond 3 d, as power within +/-0.25 d of its peak.
    double peak_fd = sidelobe_start;
    double peak_level = 0.0;
    const double half_node_rate = multiple / 2.0;

    for (double fd = sidelobe_start; fd < half_node_rate; fd += spectrum_step)
    {
        const double level = tap_psd(taps, multiple, frames, fd);

        if (level > peak_level)
        {
            peak_level = level;
            peak_fd = fd;
        }
    }

    double sidelobe = 0.0;

    for (double fd = peak_fd - sidelobe_halfwidth; fd <= peak_fd + sidelobe_halfwidth; fd += spectrum_step)
    {
        sidelobe += tap_psd(taps, multiple, frames, fd);
    }

    CHECK(signal_lab_test::power_db(sidelobe / lobe) <= sidelobe_limit_db);
    const double peak = tap_psd(taps, multiple, frames, 0.0);
    const double twenty_point = expected_sigma_per_spread * std::sqrt(2.0 * std::log(hundred));
    const double thirty_point = expected_sigma_per_spread * std::sqrt(2.0 * std::log(thousand));
    CHECK_THAT(signal_lab_test::power_db(tap_psd(taps, multiple, frames, twenty_point) / peak) + twenty_db, WithinAbs(0.0, table_e1_bound_db));
    CHECK_THAT(signal_lab_test::power_db(tap_psd(taps, multiple, frames, thirty_point) / peak) + thirty_db, WithinAbs(0.0, table_e1_bound_db));
}

TEST_CASE("doppler_filter: the taps are Rayleigh over 2^22 nodes at 1 Hz (U14)", "[doppler_filter][watterson][statistics]")
{
    const std::vector<Complex> nodes = node_sequence(one_hertz, 0U, rayleigh_nodes);
    std::vector<double> intensity;
    intensity.reserve(nodes.size());
    double power = 0.0;
    double fourth = 0.0;
    double envelope = 0.0;

    for (const Complex& node : nodes)
    {
        const double square = std::norm(node);
        intensity.push_back(square);
        power += square;
        fourth += square * square;
        envelope += std::sqrt(square);
    }

    const auto count = sl::as_double(nodes.size());
    const double mean_power = power / count;
    CHECK_THAT(mean_power, WithinAbs(1.0, power_bound));
    CHECK_THAT((fourth / count) / (mean_power * mean_power), WithinAbs(rayleigh_fourth_moment, fourth_moment_bound));
    CHECK_THAT((envelope / count) / std::sqrt(mean_power), WithinAbs(rayleigh_mean_over_rms, mean_over_rms_bound));
    // Kolmogorov-Smirnov of |h|^2 against Exp(1) at the effective sample count.
    std::ranges::sort(intensity);
    double distance = 0.0;

    for (std::size_t index = 0U; index < intensity.size(); ++index)
    {
        const double model = 1.0 - std::exp(-intensity[index]);
        const auto below = sl::as_double(index) / count;
        const auto above = sl::as_double(index + 1U) / count;
        distance = std::max({distance, std::abs(model - below), std::abs(above - model)});
    }

    const double critical = ks_critical_99 / std::sqrt(effective_nodes(nodes.size(), design_at(one_hertz).rate_multiple));
    CHECK(distance < critical);
}

TEST_CASE("doppler_filter: the streams of two paths are uncorrelated (U16)", "[doppler_filter][watterson][statistics]")
{
    const std::vector<Complex> first = node_sequence(one_hertz, 0U, spectrum_nodes);
    const std::vector<Complex> second = node_sequence(one_hertz, 1U, spectrum_nodes);
    Complex cross{};
    double first_power = 0.0;
    double second_power = 0.0;

    for (std::size_t index = 0U; index < first.size(); ++index)
    {
        cross += first[index] * std::conj(second[index]);
        first_power += std::norm(first[index]);
        second_power += std::norm(second[index]);
    }

    const double correlation = std::abs(cross) / std::sqrt(first_power * second_power);
    CHECK(correlation <= correlation_standard_errors / std::sqrt(effective_nodes(first.size(), design_at(one_hertz).rate_multiple)));
}

TEST_CASE("doppler_filter: the node spectrum gives two sigma within 3 % at 0.1, 1 and 10 Hz (U17)", "[doppler_filter][watterson][statistics]")
{
    for (const double spread : statistics_spreads_hz)
    {
        const sl::DopplerDesign design = design_at(spread);
        const std::vector<Complex> nodes = node_sequence(spread, 0U, spectrum_nodes);
        const std::vector<double> power = signal_lab_test::average_periodogram(nodes, node_segment);
        // sigma in cycles per node is (d / 2) / R; take the second moment within +/-4 sigma.
        const double node_rate = design.node_rate.in(units::Hz);
        const double extent = moment_extent_sigmas * expected_sigma_per_spread * spread / node_rate;
        double total = 0.0;
        double moment = 0.0;

        for (std::size_t bin = 0U; bin < node_segment; ++bin)
        {
            const double u = signal_lab_test::signed_bin(bin, node_segment) / sl::as_double(node_segment);

            if (std::abs(u) <= extent)
            {
                total += power[bin];
                moment += power[bin] * u * u;
            }
        }

        const double two_sigma = sigmas_per_two_sigma * std::sqrt(moment / total) * node_rate;
        INFO("d = " << spread << " Hz, two sigma " << two_sigma << " Hz");
        CHECK_THAT(two_sigma / spread, WithinAbs(1.0, spread_bound));
    }
}

TEST_CASE("doppler_filter: the closed form of v equals the brute-force interval mean (U18 a)", "[doppler_filter][watterson]")
{
    auto filter = std::make_unique<sl::DopplerFilter>();
    filter->clear();
    filter->design(design_at(one_hertz));
    const double rho = filter->lag_one_correlation();

    for (const std::uint32_t frames : frames_per_node_cases)
    {
        double brute = 0.0;

        for (std::uint32_t offset = 0U; offset < frames; ++offset)
        {
            const auto mu = sl::as_double(offset) / sl::as_double(frames);
            brute += (1.0 - mu) * (1.0 - mu) + mu * mu + cross_term_weight * mu * (1.0 - mu) * rho;
        }

        brute /= sl::as_double(frames);
        INFO("M = " << frames);
        CHECK_THAT(sl::interpolation_power(rho, units::SampleCount{frames}), WithinAbs(brute, interpolation_bound));
        CHECK(sl::interpolation_power(rho, units::SampleCount{frames}) < 1.0);
    }
}
