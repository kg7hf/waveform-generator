// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

#pragma once

// The analytic signal of the Watterson stage: a type III Kaiser Hilbert FIR at the
// stream rate (MIL-STD-188-110D E.4.2, E.6.2) and the ring of analytic samples
// that the paths read at their delays.
// Guide: docs/modules/signal-lab-watterson.md, "Hilbert filter" and "Delay ring".
//
//   xa(n) = x(n - D) + j xh(n)
//   xh(n) = sum over odd m = 1, 3, ..., D of h[m] (x(n - D - m) - x(n - D + m))
//   h[m]  = 2 / (pi m) w_K[m]          type III: antisymmetric, even offsets zero
//
// The in-phase sample is the centre tap of the same history the Hilbert reads, so
// I and Q are aligned exactly and the stage's latency is D = (N - 1) / 2 frames.
// Every size is a constexpr function of the stream rate (design 3.7).

#include "signal_lab/convert.hpp"
#include "signal_lab/kaiser.hpp"
#include "signal_lab/stream_rate.hpp"
#include "signal_lab/units/decibel.hpp"
#include "signal_lab/units/frequency.hpp"
#include "signal_lab/units/sampling.hpp"
#include "signal_lab/watterson_params.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>

namespace signal_lab
{

// The Hilbert's lower band edge: the lane's 300 Hz SNR band edge less a 50 Hz
// guard (E.6.2); the upper edge is fs / 2 less the same.
inline constexpr units::Frequency hilbert_band_edge{250.0, units::Hz};
// At least 70 dB between the wanted component and its mirror (lane criterion 4).
inline constexpr units::Ratio hilbert_attenuation{72.0, units::dB};
static_assert(kaiser::within_design_range(hilbert_attenuation), "Kaiser's A > 50 dB beta formula and the I0 series bound (beta <= 10)");

// The half-band prototype's transition is twice the band edge's distance from DC;
// the Kaiser length is then rounded up to 4k + 3, so the centre delay is odd and the
// end taps are non-zero.
[[nodiscard]] constexpr std::uint32_t hilbert_length(units::SampleRate rate) noexcept
{
    constexpr std::uint32_t type_iii_modulus = 4U;
    constexpr std::uint32_t type_iii_residue = 3U;
    const units::FrequencyDelta transition = 2.0 * (hilbert_band_edge - units::Frequency{});
    const std::uint32_t kaiser_taps = kaiser::length(hilbert_attenuation, transition, rate);
    return kaiser_taps + (type_iii_residue + type_iii_modulus - kaiser_taps % type_iii_modulus) % type_iii_modulus;
}

inline constexpr std::uint32_t hilbert_taps = hilbert_length(stream_rate);
inline constexpr std::uint32_t hilbert_delay = (hilbert_taps - 1U) / 2U;
// The non-zero taps on one side: the odd offsets 1, 3, ..., D.
inline constexpr std::uint32_t hilbert_pairs = (hilbert_delay + 1U) / 2U;
inline constexpr std::uint32_t hilbert_history = hilbert_taps - 1U;
static_assert(hilbert_taps % 4U == 3U, "type III with an odd centre delay");

// The stage's latency, D frames (design 3.4).
inline constexpr units::SampleCount hilbert_latency{hilbert_delay};

// The stage works in chunks: the Hilbert writes a whole chunk into the ring before
// any path reads it (design 4.2).
inline constexpr units::SampleCount analytic_chunk_frames{64U};
// bit_ceil(max delay + chunk): the ring covers every delay plus one chunk.
inline constexpr units::SampleCount analytic_ring_frames{std::bit_ceil(watterson_max_delay_frames.value() + analytic_chunk_frames.value())};
static_assert(analytic_ring_frames.value() >= watterson_max_delay_frames.value() + analytic_chunk_frames.value(), "design 4.2: no ring slot that a path still reads is overwritten");

// One analytic sample: the in-phase x(n - D) and the quadrature xh(n).
struct AnalyticSample
{
    float in_phase{};
    float quadrature{};
};

// The ring of analytic samples, indexed by absolute frame modulo its power-of-two size.
class AnalyticRing
{
public:
    void clear() noexcept;

    void write(units::SampleIndex frame, AnalyticSample sample) noexcept { std::span{mSamples}[slot(frame)] = sample; }

    [[nodiscard]] AnalyticSample read(units::SampleIndex frame) const noexcept { return std::span{mSamples}[slot(frame)]; }

private:
    [[nodiscard]] static constexpr std::size_t slot(units::SampleIndex frame) noexcept { return to_size(frame.value() & (analytic_ring_frames.value() - 1U)); }

    std::array<AnalyticSample, analytic_ring_frames.value()> mSamples{};
};

// The Hilbert FIR: coefficients designed at configure time, and the linear input
// buffer of 2 D history frames plus one chunk.
class AnalyticFilter
{
public:
    // Designs the coefficients with det_math (sin, and the Kaiser I0 series) and
    // clears the history. Writes every member.
    void configure() noexcept;

    // Appends input (at most one chunk, frames first, first + 1, ...) and writes the
    // analytic sample of each frame into the ring at its absolute frame.
    void process(std::span<const float> input, units::SampleIndex first, AnalyticRing& ring) noexcept;

    // The same filter and accumulation order without a ring. Writes at most one
    // analytic chunk into `output`; this is used by stages that need only the
    // current analytic samples rather than delayed path reads.
    void process(std::span<const float> input, units::SampleIndex first, std::span<AnalyticSample> output) noexcept;

    // h[m] for the odd offsets m = 2 p + 1, p = 0 ... hilbert_pairs - 1.
    [[nodiscard]] std::span<const float, hilbert_pairs> coefficients() const noexcept { return mCoefficients; }

private:
    std::array<float, hilbert_pairs> mCoefficients{};
    std::array<float, hilbert_history + analytic_chunk_frames.value()> mBuffer{};
};

} // namespace signal_lab
