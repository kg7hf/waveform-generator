// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

#pragma once

// The tap process of one Rayleigh path: complex Gaussian draws through the
// MIL-STD-188-110D E.5.4 Gaussian Doppler FIR, at a node rate of at least 64 times
// the spread. Guide: docs/modules/signal-lab-watterson.md, "Tap processes".
//
// E.5.4 in its prose form (the printed time-domain formula is the QA #7/#8 trap):
//   c_k proportional to exp(-pi^2 d^2 (k / R)^2) = exp(-pi^2 (k / K)^2),  |k| <= L,  K = R / d
//   L = ceil(tau R), tau = sqrt(ln(1 / f)) / (pi d)   the outermost tap is at most f of the peak
//   N = 2 L + 1, sum c_k^2 = 1                        unit power at the node rate
// The Doppler PSD is then Gaussian with sigma = d / 2, so d is the two-sigma spread.
// The node rate is R = fs / M with M = floor((fs / 64) / d) whole frames per node.

#include "common/status.hpp"
#include "signal_lab/complex_sample.hpp"
#include "signal_lab/convert.hpp"
#include "signal_lab/det_math.hpp"
#include "signal_lab/stream_rate.hpp"
#include "signal_lab/units/frequency.hpp"
#include "signal_lab/units/sampling.hpp"
#include "signal_lab/watterson_params.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <span>

namespace signal_lab
{

// E.5.4 prose: the Doppler PSD's sigma is half the two-sigma spread d (lane QA #7/#8).
inline constexpr double doppler_sigma_per_spread = 0.5;
// E.5.4: the tap gain at +/-tau is at most 1 % of the peak, 10^-2: two decades. The
// lane's open question Q1 (design 5.5) would make it three (0.1 %); that is a change
// of this constant only, and the capacity below follows it.
inline constexpr double doppler_truncation_decades = 2.0;
// ln(1 / f) for the truncation fraction f = 10^-decades.
inline constexpr double doppler_truncation_log = doppler_truncation_decades * std::numbers::ln10;
// The node rate is at least 64 times the spread: the survey's design input (E.5.4
// requires at least 32; 64 puts the linear-interpolation images near -80 dBc).
inline constexpr std::uint32_t tap_rate_multiple_min = 64U;
static_assert(stream_rate.hertz() % tap_rate_multiple_min == 0U, "fs / 64 is a whole number of hertz (750 at 48 kHz)");
// fs / 64: the frames per node of a 1 Hz spread (750 at 48 kHz).
inline constexpr units::FrequencyDelta doppler_node_scale{as_double(stream_rate.hertz()) / as_double(tap_rate_multiple_min), units::Hz};

// The fewest frames per node, at the largest spread: floor(750 / 10) = 75.
inline constexpr std::uint32_t doppler_min_frames_per_node = floor_to_uint32(doppler_node_scale / watterson_max_spread);
// K = R / d = 64 (750 / d) / M < 64 (M + 1) / M: its supremum is reached at the fewest
// frames per node (64.853 at 48 kHz, for d just above 750 / 76 Hz).
inline constexpr double doppler_rate_multiple_bound = as_double(tap_rate_multiple_min) * as_double(doppler_min_frames_per_node + 1U) / as_double(doppler_min_frames_per_node);

// The fewest half-taps L with exp(-(pi L / K)^2) <= f for every K below the bound.
[[nodiscard]] constexpr std::uint32_t doppler_half_capacity() noexcept
{
    constexpr std::uint32_t search_limit = 1024U;
    const double needed = doppler_truncation_log * doppler_rate_multiple_bound * doppler_rate_multiple_bound;

    for (std::uint32_t half = 0U; half < search_limit; ++half)
    {
        const double scaled = std::numbers::pi * as_double(half);

        if (scaled * scaled >= needed)
        {
            return half;
        }
    }

    return search_limit;
}

inline constexpr std::uint32_t doppler_max_half = doppler_half_capacity();
inline constexpr std::uint32_t doppler_max_taps = 2U * doppler_max_half + 1U;

// The realised design of one path's tap process.
struct DopplerDesign
{
    units::SampleCount frames_per_node{}; // M, whole frames between nodes
    units::FrequencyDelta node_rate{};    // R = fs / M
    double rate_multiple{};               // K = R / d, a pure ratio
    std::uint32_t half{};                 // L
    std::uint32_t taps{};                 // N = 2 L + 1
};

// The tap rate and FIR length for a spread in [0.1, 10] Hz (watterson_params.hpp).
// Refused (invalid_argument, a static message) for a spread outside that range or
// not finite, a rate below 64 Hz, or a design beyond the static capacity
// doppler_max_half: never a wrapped cast or a silently clipped filter.
[[nodiscard]] m110::Result<DopplerDesign> design_doppler(units::FrequencyDelta spread, units::SampleRate rate) noexcept;

// The mean power of the linearly interpolated tap, for a unit-power node process with
// lag-one correlation rho1 and M frames per node (design 6, closed form):
//   v = 1 - (1 - rho1) (M^2 - 1) / (3 M^2)
[[nodiscard]] double interpolation_power(double lag_one_correlation, units::SampleCount frames_per_node) noexcept;

// One Rayleigh path's Gaussian FIR, its mirrored draw history and its PCG32 stream.
class DopplerFilter
{
public:
    // Zeroes every member in place (no temporary of the ~1.7 KiB object).
    void clear() noexcept;

    // Designs the unit-energy FIR for `design` (det::exp), then stores it as float.
    // Writes the coefficients; the history is written by prime().
    void design(const DopplerDesign& design) noexcept;

    // rho1 = sum c_k c_(k+1) / sum c_k^2 of the stored float coefficients, in double.
    [[nodiscard]] double lag_one_correlation() const noexcept;

    // Fills the history with N draws from `generator` and returns node 0. Each node is
    // the FIR output times `amplitude` (the path's a_i over the root energy of the
    // stored coefficients), so the coefficients stay the filter that rho1 describes.
    [[nodiscard]] ComplexFloat prime(const det::Pcg32& generator, float amplitude) noexcept;

    // One complex draw into the history, and the next node.
    [[nodiscard]] ComplexFloat next_node() noexcept;

    // c_k from the outermost tap (k = 0) to the centre (k = L).
    [[nodiscard]] std::span<const float> half_coefficients() const noexcept { return std::span{mCoefficients}.first(mHalf + 1U); }

    [[nodiscard]] std::uint32_t taps() const noexcept { return mTaps; }

    // sum c_k^2 of the stored float coefficients, in double.
    [[nodiscard]] double energy() const noexcept;

private:
    void push(ComplexFloat draw) noexcept;
    [[nodiscard]] ComplexFloat filter() const noexcept;
    [[nodiscard]] ComplexFloat draw() noexcept;

    std::array<float, doppler_max_half + 1U> mCoefficients{};
    std::array<ComplexFloat, std::size_t{2} * doppler_max_taps> mHistory{}; // mirrored: every draw twice
    det::Pcg32 mGenerator{};
    std::uint32_t mHalf{};
    std::uint32_t mTaps{};
    std::uint32_t mOldest{};
    float mAmplitude{};
};

} // namespace signal_lab
