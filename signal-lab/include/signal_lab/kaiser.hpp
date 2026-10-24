// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

#pragma once

// The Kaiser window and its design formulas (J. F. Kaiser, "Nonrecursive digital
// filter design using the I0-sinh window function", Proc. IEEE ISCAS, 1974).
// Guide: docs/modules/signal-lab-watterson.md, "Hilbert filter".
//
// The Watterson stage's Hilbert FIR (72 dB) and the noise band of the AWGN stage
// (60 dB) are Kaiser designs. beta() and length() are constexpr, so the filter
// lengths size their arrays at compile time; the window itself is evaluated at
// configure time with the I0 series below (no C library), the same on host and board.

#include "signal_lab/convert.hpp"
#include "signal_lab/units/decibel.hpp"
#include "signal_lab/units/frequency.hpp"
#include "signal_lab/units/sampling.hpp"

#include <cstdint>
#include <numbers>

namespace signal_lab::kaiser
{

// Kaiser (1974), for a stopband attenuation A > 50 dB: beta = 0.1102 (A - 8.7).
// The formula takes A as a number of decibels.
inline constexpr double beta_slope = 0.1102;
inline constexpr double beta_offset_db = 8.7;
// Below this attenuation Kaiser gives a different beta formula, which no stage uses:
// within_design_range() refuses it, and every design asserts that at compile time.
inline constexpr double beta_formula_minimum_db = 50.0;
// Kaiser (1974): N - 1 = (A - 7.95) / (2.285 dw), dw the transition in rad/sample.
inline constexpr double length_offset_db = 7.95;
inline constexpr double length_slope = 2.285;
// Terms of the I0 power series: for beta <= 10 the next term is below 1e-29 of the sum.
inline constexpr std::uint32_t series_terms = 32U;
// The largest beta that series_terms is stated for (the bound above).
inline constexpr double series_beta_limit = 10.0;

[[nodiscard]] constexpr double beta(units::Ratio attenuation) noexcept
{
    const double decibels = attenuation.in(units::dB);
    return beta_slope * (decibels - beta_offset_db);
}

// True when beta() and bessel_i0() hold for this attenuation: above Kaiser's 50 dB
// formula boundary, and a beta within the I0 series bound. Each Kaiser design states
// static_assert(within_design_range(its attenuation)) beside its constant.
[[nodiscard]] constexpr bool within_design_range(units::Ratio attenuation) noexcept
{
    return attenuation.in(units::dB) > beta_formula_minimum_db && beta(attenuation) <= series_beta_limit;
}

// The Kaiser length N for a transition width, before any rounding to a filter type.
[[nodiscard]] constexpr std::uint32_t length(units::Ratio attenuation, units::FrequencyDelta transition, units::SampleRate rate) noexcept
{
    const double decibels = attenuation.in(units::dB);
    const double transition_radians = 2.0 * std::numbers::pi * (transition.in(units::Hz) / as_double(rate.hertz()));
    return ceil_to_uint32((decibels - length_offset_db) / (length_slope * transition_radians)) + 1U;
}

// The modified Bessel function of the first kind, order 0, by its power series
// sum_k ((x/2)^k / k!)^2, with series_terms terms in a fixed order.
[[nodiscard]] double bessel_i0(double x) noexcept;

// The window at a position in [-1, 1] across the filter: I0(beta sqrt(1 - p^2)) / I0(beta).
[[nodiscard]] double window(double beta, double position) noexcept;

} // namespace signal_lab::kaiser
