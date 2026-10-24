// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

#pragma once

// Block-streaming impairment interface shared by the host renderer and the
// RT1170 player. A stage processes float blocks in place (values scaled so
// that +/-1.0 is PCM full scale), keyed by the absolute index of the block's
// first *input* frame so results never depend on block boundaries. Stages
// are placement-constructed into caller-owned storage: no heap, no exceptions.

#include "common/status.hpp"
#include "signal_lab/scenario.hpp"
#include "signal_lab/stream_identity.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>

namespace signal_lab
{

// 2048 frames = 42.7 ms per block, the RT1170 player's 4 KiB SD chunk. Slack covers
// duplicated runs (sample slips) that lengthen a block.
inline constexpr std::size_t engine_block_frames = 2048U;
inline constexpr std::size_t engine_slack_frames = 256U;
inline constexpr std::size_t engine_capacity_frames = engine_block_frames + engine_slack_frames;
// The legacy stages are placement-constructed back to back in one static arena (no
// heap). 28 KiB holds the arena part of a representative largest chain (fade,
// CW, impulse, sample slip); its AWGN stage lives in a typed engine slot (awgn.hpp).
inline constexpr std::size_t impairment_arena_bytes = 28672U;
inline constexpr std::size_t impairment_alignment = 16U;
inline constexpr std::uint64_t unbounded_frames = 0xFFFFFFFFFFFFFFFFULL;

struct StageContext
{
    double reference_rms{};       // RMS of the (already gain-scaled) desired signal
    std::uint64_t total_frames{}; // 0 when the stream length is unknown (live sources)
    std::uint64_t seed{};
    std::uint32_t index{};
    float* scratch{};                                     // engine-owned, engine_capacity_frames floats, valid during process()
    NoiseBandwidth noise_bandwidth{NoiseBandwidth::khz3}; // scenario-wide AWGN and SNR band
    std::uint32_t stream_id{};                            // zero preserves stage-index keyed legacy streams

    [[nodiscard]] constexpr StageRandomKey random_key() const noexcept { return resolve_stage_random_key(seed, index, stream_id); }
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
    // Readies the stage for a stream: success, or why it cannot run, a Status
    // with a static message (invalid_configuration for parameters the stage
    // refuses; the Watterson stage returns its channel's own Status).
    [[nodiscard]] virtual m110::Status prepare(const StageContext& context) noexcept = 0;
    // The number of frames now in the block (differs from `frames` only for sample slips),
    // or the stage's fault: a Status with a static message (the Watterson channel's
    // contiguity and configuration checks; the other stages cannot fail mid-stream). The
    // engine latches a fault as its own status (Engine::status()), so it reaches the caller.
    [[nodiscard]] virtual m110::Result<std::size_t> process(float* block, std::size_t frames, std::uint64_t first_input_frame, std::size_t capacity) noexcept = 0;
    [[nodiscard]] virtual const StageStats& stats() const noexcept = 0;

protected:
    // Only an implementation copies or moves itself, never through this interface (C.67).
    Impairment() = default;
    Impairment(const Impairment&) = default;
    Impairment& operator=(const Impairment&) = default;
    Impairment(Impairment&&) = default;
    Impairment& operator=(Impairment&&) = default;
};

// Window helper: [start, end) in frames; end is unbounded_frames when open-ended.
void resolve_window(const Window& window, std::uint64_t total_frames, std::uint64_t& start, std::uint64_t& end) noexcept;

// Storage needed by each implementation (rounded up to impairment_alignment).
// 0 for the stages the engine builds in typed slots instead (AWGN, Watterson).
[[nodiscard]] std::size_t impairment_size(StageType type) noexcept;
[[nodiscard]] std::size_t cw_size() noexcept;
[[nodiscard]] std::size_t impulse_size() noexcept;
[[nodiscard]] std::size_t fade_size() noexcept;
[[nodiscard]] std::size_t sample_slip_size() noexcept;

constexpr std::size_t align_storage(std::size_t bytes) noexcept
{
    return (bytes + impairment_alignment - 1U) / impairment_alignment * impairment_alignment;
}

// A placement constructor's stage: the Impairment built in the caller's storage.
using PlacedStage = m110::Result<std::reference_wrapper<Impairment>>;

// What a placement constructor refuses: storage that is null, not aligned to
// impairment_alignment or smaller than impairment_size(type).
inline constexpr m110::Status placement_refused{.code = m110::StatusCode::buffer_too_small, .message = "impairment storage is null, misaligned or smaller than the stage"};
// construct_impairment()'s refusal of a typed-slot stage (AWGN, Watterson), which
// the engine builds in its own slot.
inline constexpr m110::Status not_an_arena_stage{.code = m110::StatusCode::invalid_argument, .message = "impairment type is built in a typed engine slot, not in the arena"};

// Placement constructors: the stage, or placement_refused (construct_impairment:
// not_an_arena_stage for AWGN and Watterson).
[[nodiscard]] PlacedStage construct_cw(const CwParams& params, void* storage, std::size_t storage_bytes) noexcept;
[[nodiscard]] PlacedStage construct_impulse(const ImpulseParams& params, void* storage, std::size_t storage_bytes) noexcept;
[[nodiscard]] PlacedStage construct_fade(const FadeParams& params, void* storage, std::size_t storage_bytes) noexcept;
[[nodiscard]] PlacedStage construct_sample_slip(const SampleSlipParams& params, void* storage, std::size_t storage_bytes) noexcept;
[[nodiscard]] PlacedStage construct_impairment(const Stage& stage, void* storage, std::size_t storage_bytes) noexcept;

} // namespace signal_lab
