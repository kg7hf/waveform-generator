// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Paul R. Decker

#pragma once

// Block-streaming impairment interface shared by the host renderer and the
// RT1170 player. A stage processes float blocks in place (values scaled so
// that +/-1.0 is PCM full scale), keyed by the absolute index of the block's
// first *input* frame so results never depend on block boundaries. Stages
// are placement-constructed into caller-owned storage: no heap, no exceptions.

#include "signal_lab/scenario.hpp"

#include <cstddef>
#include <cstdint>

namespace signal_lab
{

// 2048 frames = 42.7 ms per block, the RT1170 player's 4 KiB SD chunk. Slack covers
// duplicated runs (sample slips) that lengthen a block.
inline constexpr std::size_t engine_block_frames = 2048U;
inline constexpr std::size_t engine_slack_frames = 256U;
inline constexpr std::size_t engine_capacity_frames = engine_block_frames + engine_slack_frames;
// Stages are placement-constructed back to back in one static arena (no heap).
// 32 KiB holds the five-stage MIX-005 chain on the RT1170's DTCM.
inline constexpr std::size_t impairment_arena_bytes = 32768U;
inline constexpr std::size_t impairment_alignment = 16U;
inline constexpr std::uint64_t unbounded_frames = 0xFFFFFFFFFFFFFFFFULL;

struct StageContext
{
    double reference_rms{};       // RMS of the (already gain-scaled) desired signal
    std::uint64_t total_frames{}; // 0 when the stream length is unknown (live sources)
    std::uint64_t seed{};
    std::uint32_t index{};
    float* scratch{};             // engine-owned, engine_capacity_frames floats, valid during process()
};

struct StageStats
{
    std::uint32_t events_scheduled{};
    std::uint32_t events_applied{};
    std::uint32_t events_dropped{};
    float component_peak{};
    double component_energy{};
    std::uint64_t component_frames{};
};

class Impairment
{
public:
    virtual ~Impairment() = default;
    [[nodiscard]] virtual StageType type() const noexcept = 0;
    [[nodiscard]] virtual bool prepare(const StageContext& context, const char** error) noexcept = 0;
    // Returns the number of frames now in the block (differs from `frames` only for sample slips).
    [[nodiscard]] virtual std::size_t process(float* block, std::size_t frames, std::uint64_t first_input_frame, std::size_t capacity) noexcept = 0;
    [[nodiscard]] virtual const StageStats& stats() const noexcept = 0;
};

// Window helper: [start, end) in frames; end is unbounded_frames when open-ended.
void resolve_window(const Window& window, std::uint64_t total_frames, std::uint64_t& start, std::uint64_t& end) noexcept;

// Storage needed by each implementation (rounded up to impairment_alignment).
[[nodiscard]] std::size_t impairment_size(StageType type) noexcept;
[[nodiscard]] std::size_t awgn_size() noexcept;
[[nodiscard]] std::size_t cw_size() noexcept;
[[nodiscard]] std::size_t impulse_size() noexcept;
[[nodiscard]] std::size_t fade_size() noexcept;
[[nodiscard]] std::size_t sample_slip_size() noexcept;

constexpr std::size_t align_storage(std::size_t bytes) noexcept
{
    return (bytes + impairment_alignment - 1U) / impairment_alignment * impairment_alignment;
}

// Placement constructors; each returns nullptr when storage_bytes < impairment_size(type).
[[nodiscard]] Impairment* construct_awgn(const AwgnParams& params, void* storage, std::size_t storage_bytes) noexcept;
[[nodiscard]] Impairment* construct_cw(const CwParams& params, void* storage, std::size_t storage_bytes) noexcept;
[[nodiscard]] Impairment* construct_impulse(const ImpulseParams& params, void* storage, std::size_t storage_bytes) noexcept;
[[nodiscard]] Impairment* construct_fade(const FadeParams& params, void* storage, std::size_t storage_bytes) noexcept;
[[nodiscard]] Impairment* construct_sample_slip(const SampleSlipParams& params, void* storage, std::size_t storage_bytes) noexcept;
[[nodiscard]] Impairment* construct_impairment(const Stage& stage, void* storage, std::size_t storage_bytes) noexcept;

} // namespace signal_lab
