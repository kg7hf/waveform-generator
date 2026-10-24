// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

#pragma once

// Fixed-capacity source overlay for host/RT1170 callers. The source and params
// are borrowed and must outlive this stage; no source samples are retained.
// After a provider, filter, or output-processing failure, discard the block and
// call prepare() before processing again.

#include "signal_lab/frequency_shift.hpp"
#include "signal_lab/impairment.hpp"
#include "signal_lab/second_source_params.hpp"
#include "signal_lab/secondary_source.hpp"
#include "signal_lab/watterson.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <span>

namespace signal_lab
{

inline constexpr std::uint32_t second_source_algorithm_version = 1U;

class SecondSourceStage final : public Impairment
{
public:
    SecondSourceStage(const SecondSourceParams& params, SecondarySource* source) noexcept : mParams{&params}, mSource{source} {}

    ~SecondSourceStage() override = default;
    SecondSourceStage(SecondSourceParams&&, SecondarySource*) = delete;
    SecondSourceStage(const SecondSourceStage&) = delete;
    SecondSourceStage& operator=(const SecondSourceStage&) = delete;
    SecondSourceStage(SecondSourceStage&&) = delete;
    SecondSourceStage& operator=(SecondSourceStage&&) = delete;

    [[nodiscard]] StageType type() const noexcept override { return StageType::second_source; }

    [[nodiscard]] m110::Status prepare(const StageContext& context) noexcept override;
    [[nodiscard]] m110::Result<std::size_t> process(float* block, std::size_t frames, std::uint64_t first_input_frame, std::size_t capacity) noexcept override;
    [[nodiscard]] const StageStats& stats() const noexcept override { return mStats; }

private:
    [[nodiscard]] m110::Status prepare_source(const StageContext& context) noexcept;
    [[nodiscard]] m110::Status read_active(std::uint64_t first, std::span<float> output) noexcept;
    [[nodiscard]] m110::Status process_offset(FrequencyShiftStage& shifter, float* block, std::size_t frames, std::uint64_t first_input_frame) noexcept;
    [[nodiscard]] m110::Status prepare_fading(const StageContext& context) noexcept;
    [[nodiscard]] m110::Status process_active_shifter(FrequencyShiftStage& shifter, float* samples, std::uint64_t local_first, std::size_t count) noexcept;
    [[nodiscard]] m110::Status process_unshifted(float* block, std::size_t frames, std::uint64_t first_input_frame) noexcept;
    [[nodiscard]] m110::Status add_component(float* block, std::span<const float> component, std::uint64_t first_input_frame) noexcept;

    const SecondSourceParams* mParams{};
    SecondarySource* mSource{};
    float* mScratch{};
    std::uint64_t mSourceFrames{};
    std::uint64_t mActiveFrames{};
    std::uint64_t mActiveEnd{};
    std::uint64_t mNextFrame{};
    double mScale{};
    FrequencyShiftParams mShiftParams{};
    std::optional<FrequencyShiftStage> mShifter{};
    WattersonPathState mFadingState{};
    WattersonPath mFadingPath{};
    WattersonPathReport mFadingReport{};
    MarkovGate mMarkovGate{};
    std::array<ComplexFloat, analytic_chunk_frames.value()> mGainChunk{};
    std::uint64_t mFadingNextFrame{};
    StageStats mStats{};
    m110::Status mFaultStatus{};
    bool mHaveFrame{};
    bool mMarkovEnabled{};
    bool mPrepared{};
};

} // namespace signal_lab
