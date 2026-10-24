// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

#pragma once

// Global carrier offset and triangular Doppler sweep, using the same analytic
// signal design as Watterson and a continuous 64-bit phase accumulator. This
// standalone analytic stage adds the Hilbert filter's fixed 215-frame latency.

#include "signal_lab/analytic_filter.hpp"
#include "signal_lab/complex_sample.hpp"
#include "signal_lab/frequency_shift_params.hpp"
#include "signal_lab/impairment.hpp"
#include "signal_lab/stream_rate.hpp"

#include <array>
#include <cstdint>
#include <span>

namespace signal_lab
{

// Validates parameters shared by parsed scenarios and direct callers.
[[nodiscard]] m110::Status validate_frequency_shift_params(const FrequencyShiftParams& params) noexcept;

// The instantaneous frequency deviation at an absolute frame index. The
// triangle's cycle rate is quantized once to Q0.64, then its phase is computed
// as start_phase + frame * cycle_step modulo one turn. No integer half-period
// rounding is used, avoiding half-period rounding drift. With the accepted
// cycle-step limit, cycle-step rounding bounds frequency error below 3e-7 Hz
// through frame 2^32 at the maximum amplitude; beyond that bound grows linearly.
// Callers must pass valid params and a non-zero rate.
[[nodiscard]] units::FrequencyDelta instantaneous_frequency(const FrequencyShiftParams& params, units::SampleIndex frame, units::SampleRate rate = stream_rate) noexcept;

class FrequencyShiftStage final : public Impairment
{
public:
    explicit FrequencyShiftStage(const FrequencyShiftParams& params) noexcept : mParams{&params} {}

    [[nodiscard]] StageType type() const noexcept override { return StageType::frequency_shift; }

    [[nodiscard]] m110::Status prepare(const StageContext& context) noexcept override;
    [[nodiscard]] m110::Result<std::size_t> process(float* block, std::size_t frames, std::uint64_t first_input_frame, std::size_t capacity) noexcept override;
    [[nodiscard]] m110::Result<std::size_t> process_with_gain(float* block, std::size_t frames, std::uint64_t first_input_frame, std::size_t capacity, std::span<const ComplexFloat> gains) noexcept;
    [[nodiscard]] const StageStats& stats() const noexcept override { return mStats; }

    [[nodiscard]] std::uint64_t phase_word() const noexcept { return mPhase; }

private:
    [[nodiscard]] m110::Result<std::size_t> process_impl(float* block, std::size_t frames, std::uint64_t first_input_frame, std::size_t capacity, std::span<const ComplexFloat> gains,
                                                         bool apply_gain) noexcept;
    const FrequencyShiftParams* mParams{};
    AnalyticFilter mFilter{};
    std::array<AnalyticSample, analytic_chunk_frames.value()> mAnalytic{};
    StageStats mStats{};
    std::uint64_t mNextFrame{};
    std::uint64_t mPhase{};
    std::uint64_t mSweepPhase{};
    std::uint64_t mSweepStartPhase{};
    std::uint64_t mSweepStep{};
    bool mHaveFrame{};
    bool mPrepared{};
};

} // namespace signal_lab
