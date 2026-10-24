// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// Catch2 suite of the Kaiser window (signal_lab/kaiser.hpp): design 3.3 and 7.3, test U3.
// Guide: docs/modules/signal-lab-watterson.md, "Hilbert filter".

#include "signal_lab/analytic_filter.hpp"
#include "signal_lab/kaiser.hpp"
#include "signal_lab/stream_rate.hpp"
#include "signal_lab/units/decibel.hpp"
#include "signal_lab/units/frequency.hpp"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cstdint>

namespace
{

namespace kaiser = signal_lab::kaiser;
namespace units = signal_lab::units;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

// I0 reference values (numpy.i0, Cephes' Chebyshev expansion: an independent method).
struct BesselCase
{
    double x;
    double i0;
};

constexpr std::array bessel_cases{
    BesselCase{0.0, 1.0},
    BesselCase{1.0, 1.2660658777520082},
    BesselCase{5.0, 27.239871823604442},
    BesselCase{5.65326, 49.04845930258202}, // the noise band's beta (A = 60 dB)
    BesselCase{6.97566, 164.8389239086097}, // the Hilbert's beta (A = 72 dB)
    BesselCase{10.0, 2815.716628466254},
};
// Both methods are good to a few ulp; the series sums 32 positive terms.
constexpr double bessel_bound = 1.0e-14;

// Kaiser (1974): beta = 0.1102 (A - 8.7) for the two designs.
constexpr double hilbert_beta = 6.97566;
constexpr double noise_beta = 5.65326;
constexpr double beta_bound = 1.0e-13;
constexpr double noise_attenuation_db = 60.0;
constexpr double noise_transition_hz = 300.0;
// N - 1 = (A - 7.95) / (2.285 dw): 430 for the Hilbert (rounded up to 431, 4k + 3) and
// 582 for the noise band (rounded up to 583, odd), design 3.3 and 7.3.
constexpr std::uint32_t hilbert_kaiser_length = 430U;
constexpr std::uint32_t hilbert_type_iii_length = 431U;
constexpr std::uint32_t noise_kaiser_length = 582U;
// The window is 1 at its centre and I0(0) / I0(beta) at its ends.
constexpr double window_bound = 1.0e-15;
// Outside the design range: Kaiser's other beta formula (A <= 50 dB), and a beta of
// 12.3 (A = 120 dB) beyond the I0 series bound.
constexpr double other_formula_db = 40.0;
constexpr double formula_edge_db = 50.0;
constexpr double beyond_series_db = 120.0;

} // namespace

TEST_CASE("kaiser: the I0 series matches reference values (U3)", "[kaiser]")
{
    for (const BesselCase& reference : bessel_cases)
    {
        INFO("x = " << reference.x);
        CHECK_THAT(kaiser::bessel_i0(reference.x), WithinRel(reference.i0, bessel_bound));
    }
}

TEST_CASE("kaiser: beta and length follow Kaiser (1974) and give 431 and 583 taps (U3)", "[kaiser]")
{
    CHECK_THAT(kaiser::beta(signal_lab::hilbert_attenuation), WithinRel(hilbert_beta, beta_bound));
    CHECK_THAT(kaiser::beta(units::Ratio{noise_attenuation_db, units::dB}), WithinRel(noise_beta, beta_bound));
    const units::FrequencyDelta hilbert_transition = 2.0 * (signal_lab::hilbert_band_edge - units::Frequency{});
    CHECK(kaiser::length(signal_lab::hilbert_attenuation, hilbert_transition, signal_lab::stream_rate) == hilbert_kaiser_length);
    CHECK(signal_lab::hilbert_taps == hilbert_type_iii_length);
    CHECK(kaiser::length(units::Ratio{noise_attenuation_db, units::dB}, units::FrequencyDelta{noise_transition_hz, units::Hz}, signal_lab::stream_rate) == noise_kaiser_length);
}

TEST_CASE("kaiser: the design range holds both designs and refuses the other formula and a beta above 10", "[kaiser]")
{
    STATIC_CHECK(kaiser::within_design_range(signal_lab::hilbert_attenuation));
    STATIC_CHECK(kaiser::within_design_range(units::Ratio{noise_attenuation_db, units::dB}));
    STATIC_CHECK_FALSE(kaiser::within_design_range(units::Ratio{other_formula_db, units::dB}));
    STATIC_CHECK_FALSE(kaiser::within_design_range(units::Ratio{formula_edge_db, units::dB}));
    STATIC_CHECK_FALSE(kaiser::within_design_range(units::Ratio{beyond_series_db, units::dB}));
}

TEST_CASE("kaiser: the window is 1 at its centre, symmetric and I0(0) / I0(beta) at its ends", "[kaiser]")
{
    constexpr double position = 0.37;
    CHECK(kaiser::window(hilbert_beta, 0.0) == 1.0);
    CHECK(kaiser::window(hilbert_beta, position) == kaiser::window(hilbert_beta, -position));
    CHECK_THAT(kaiser::window(hilbert_beta, 1.0), WithinAbs(1.0 / kaiser::bessel_i0(hilbert_beta), window_bound));
}
