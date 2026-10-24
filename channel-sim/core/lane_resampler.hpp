// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// The HF lane's 48 kHz <-> 8 kHz resampler, streamed (guide: docs/modules/pathsim.md).
//
// The lane's chain (run_pathsim.py) converts with m110_wav_resample (snapshot
// wave3-final, e9268ae6, built from compliance/app_decode/wav_resample_fast.cpp,
// byte-identical to the reference wav_resample.cpp). Every output sample i is a
// normalized, Blackman-windowed sinc sum over 192 input samples:
//   p = i * (in_rate / out_rate),  c = floor(p),  taps j = c - 95 .. c + 96 within the input
//   d = p - j,  u = d / 96;  taps with |u| >= 1 are skipped
//   w = 2 fc sinc(2 fc d) (0.42 + 0.5 cos(pi u) + 0.08 cos(2 pi u)),  fc = 0.45 min(1, out/in)
//   y = sum(x_j w) / sum(w)  (0 if |sum(w)| <= 1e-12),  then y *= gain
//   PCM16 = lround(clamp(y * 32767, -32768, 32767))
// over N inputs it writes llround(N * out / in) outputs.
//
// LaneResampler evaluates exactly those operations in that order, so on the same C
// runtime it is bit-identical for finite input. Two facts make it fast without
// changing a bit:
//  - d = fl(p - j) equals fl(f - t) for f = p - c (exact) and t = j - c, so the 192
//    weights and their in-order sum depend only on f. They are cached per f value
//    (48 -> 8 kHz has one f; 8 -> 48 kHz has six phases with a few rounding variants).
//  - a skipped tap and a tap of weight +0.0 give the same sums (s + 0 = s, and neither
//    sum can be -0), so an interior output runs all 192 taps without branches.
// Taps outside the input (the first and last outputs) are skipped as in the lane tool.
// The second fact needs finite samples (0 * inf is NaN where the tool skips the tap),
// so push() refuses inf and NaN.
//
// Streaming: the resampler keeps the last 256 inputs. The caller pushes one input
// at a time and must take every ready output first; finish() marks the end of the
// input, after which the remaining outputs (up to the count above) become ready.

#pragma once

#include "common/status.hpp"
#include "core/pcm.hpp"
#include "core/sample_stream.hpp"
#include "core/units.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace channel_sim
{

inline constexpr std::size_t lane_filter_radius = 96U;
inline constexpr std::size_t lane_filter_taps = 2U * lane_filter_radius;
// The lane tool accepts output rates of 8000..192000 Hz; inputs are 8 or 48 kHz.
inline constexpr SampleRate lane_lowest_rate{8000U};
inline constexpr SampleRate lane_highest_rate{192000U};

class LaneResampler
{
public:
    static constexpr std::size_t history_capacity = 256U;
    static constexpr std::size_t kernel_cache_entries = 16U;

    // gain is the linear factor applied before quantization (the tool's 10^(dB/20)).
    [[nodiscard]] m110::Status configure(SampleRate input, SampleRate output, double gain) noexcept;
    // Forgets the input and output position; keeps the configuration and kernels.
    void restart() noexcept;

    // Appends the next input sample. Fails while an output is ready, after finish(),
    // for a sample that is not finite, and beyond 2^56 / 24 inputs (so that every
    // count and index stays exact in convert.hpp's conversions).
    [[nodiscard]] m110::Status push(double sample) noexcept;
    // The input has ended.
    void finish() noexcept;

    [[nodiscard]] bool ready() const noexcept;
    [[nodiscard]] bool done() const noexcept;
    // The next output; fails unless ready().
    [[nodiscard]] m110::Result<Pcm16> next() noexcept;

    [[nodiscard]] std::uint64_t inputs() const noexcept;
    [[nodiscard]] std::uint64_t outputs() const noexcept;
    [[nodiscard]] std::uint64_t kernel_misses() const noexcept;

private:
    struct Kernel
    {
        double frac{};
        double weight_sum{};
        std::array<double, lane_filter_taps> weights{};
    };

    [[nodiscard]] const Kernel& kernel_for(double frac) noexcept;
    void compute_kernel(Kernel& kernel, double frac) const noexcept;
    void locate_next() noexcept;
    [[nodiscard]] double interior_value(const Kernel& kernel, std::uint64_t first) const noexcept;
    [[nodiscard]] double edge_value(const Kernel& kernel, std::int64_t first) const noexcept;

    SampleRate input_rate_{};
    SampleRate output_rate_{};
    double input_per_output_{};
    double two_cutoff_{};
    double gain_{1.0};
    bool configured_{};

    std::array<double, history_capacity> history_{};
    std::uint64_t pushed_{};
    std::uint64_t produced_{};
    std::uint64_t output_count_{};
    bool finished_{};
    double position_{};
    std::int64_t center_{};

    std::array<Kernel, kernel_cache_entries> kernels_{};
    std::size_t kernels_used_{};
    std::size_t next_victim_{};
    std::size_t hint_{};
    std::uint64_t kernel_misses_{};
};

static_assert((LaneResampler::history_capacity & (LaneResampler::history_capacity - 1U)) == 0U, "history is a power-of-two ring");
static_assert(LaneResampler::history_capacity > lane_filter_taps + 1U, "history holds one output's taps plus the next input");

// 48 kHz unit samples in, the lane's 8 kHz PCM16 out (chain steps 1-2: resample with
// the campaign gain; the campaign's PCM16 rewrite changes no sample). Rewinding
// rewinds the upstream source and restarts the resampler.
class LaneDownsampler final : public Pcm16Source
{
public:
    // Borrows upstream until the next attach().
    [[nodiscard]] m110::Status attach(UnitSource& upstream, SampleRate input, SampleRate output, double gain) noexcept;

    [[nodiscard]] m110::Result<std::size_t> read(std::span<Pcm16> destination) noexcept override;
    [[nodiscard]] m110::Status rewind() noexcept override;

    [[nodiscard]] const LaneResampler& resampler() const noexcept;

private:
    static constexpr std::size_t staging_samples = 1024U;

    [[nodiscard]] m110::Status feed() noexcept;

    UnitSource* upstream_{};
    LaneResampler resampler_{};
    std::array<double, staging_samples> staging_{};
    std::size_t staged_{};
    std::size_t consumed_{};
    bool upstream_ended_{};
};

// pathsim's 8 kHz PCM16 in (read back as s / 32768, as the tool reads the campaign's
// rewrite), the lane's 48 kHz PCM16 out (chain steps 4-5).
class LaneUpsampler final : public Pcm16Sink
{
public:
    // Borrows downstream until the next attach().
    [[nodiscard]] m110::Status attach(Pcm16Sink& downstream, SampleRate input, SampleRate output, double gain) noexcept;

    [[nodiscard]] m110::Status write(std::span<const Pcm16> samples) noexcept override;
    // The input has ended: writes the remaining outputs downstream.
    [[nodiscard]] m110::Status finish() noexcept;

    [[nodiscard]] const LaneResampler& resampler() const noexcept;

private:
    static constexpr std::size_t staging_samples = 4096U;

    [[nodiscard]] m110::Status drain() noexcept;
    [[nodiscard]] m110::Status flush() noexcept;

    Pcm16Sink* downstream_{};
    LaneResampler resampler_{};
    std::array<Pcm16, staging_samples> staging_{};
    std::size_t staged_{};
};

} // namespace channel_sim
