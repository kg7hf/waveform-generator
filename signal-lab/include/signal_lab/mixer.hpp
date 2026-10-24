// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

#pragma once

// The engine: normalized float in -> gain -> ordered impairment stages -> clip
// -> normalized float out. PCM16 remains as an explicit legacy adapter.

#include "common/status.hpp"
#include "signal_lab/awgn.hpp"
#include "signal_lab/det_math.hpp"
#include "signal_lab/frequency_shift.hpp"
#include "signal_lab/impairment.hpp"
#include "signal_lab/scenario.hpp"
#include "signal_lab/second_source.hpp"
#include "signal_lab/stage_resources.hpp"
#include "signal_lab/watterson.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string_view>

namespace signal_lab
{

// The engine's own failures, as Engine::process() returns and latches them. A
// stage's fault is that stage's own Status (Impairment::process()), latched the
// same way. Each message is static text.
namespace engine_failure
{

inline constexpr m110::Status not_configured{.code = m110::StatusCode::unavailable, .message = "engine: process() before a successful configure()"};
inline constexpr m110::Status invalid_block{.code = m110::StatusCode::invalid_argument, .message = "engine: a null buffer, a block over engine_block_frames or an output without engine_slack_frames"};
inline constexpr m110::Status nonfinite_input{.code = m110::StatusCode::invalid_argument, .message = "engine: a non-finite input sample"};
inline constexpr m110::Status nonfinite_output{.code = m110::StatusCode::internal_error, .message = "engine: a stage produced a non-finite sample"};
// The scenario's clip_policy is reject and the output clipped.
inline constexpr m110::Status clipping{.code = m110::StatusCode::invalid_configuration, .message = "engine: output clipped with clip_policy reject"};
// A stage lengthened a block past the capacity it was given: an engine
// invariant the stage broke (only sample slips lengthen a block, into the slack).
inline constexpr m110::Status block_overrun{.code = m110::StatusCode::internal_error, .message = "engine: a stage lengthened a block past its capacity"};

inline constexpr std::array all{not_configured, invalid_block, nonfinite_input, nonfinite_output, clipping, block_overrun};

} // namespace engine_failure

// Whether `status` is `failure`: the same code and the same message text. A
// message is compared by its text, not by its address, as rt1170/error_map.hpp
// compares a Status with its table.
[[nodiscard]] constexpr bool same_failure(m110::Status status, m110::Status failure) noexcept
{
    return status.code == failure.code && status.message != nullptr && failure.message != nullptr && std::string_view{status.message} == failure.message;
}

// Whether `status` is one of the engine's own failures rather than a stage's fault.
[[nodiscard]] constexpr bool is_engine_failure(m110::Status status) noexcept
{
    for (const m110::Status failure : engine_failure::all)
    {
        if (same_failure(status, failure))
        {
            return true;
        }
    }

    return false;
}

static_assert(same_failure(engine_failure::clipping, engine_failure::clipping) && !same_failure(engine_failure::clipping, engine_failure::nonfinite_output), "a failure is its code and its message");
static_assert(is_engine_failure(engine_failure::block_overrun) && !is_engine_failure(m110::Status::success()), "the engine's own failures");

struct EngineStats
{
    std::uint64_t frames_in{};
    std::uint64_t frames_out{};
    std::uint64_t clipped_samples{};
    std::uint64_t first_clipped_frame{0xFFFFFFFFFFFFFFFFULL};
    float peak{};                  // Absolute peak in normalized full-scale units.
    double output_energy{};        // Sum of squares in normalized full-scale units.
    std::uint64_t output_digest{}; // FNV-1a 64 over the emitted canonical PCM stream
    std::uint64_t source_digest{}; // FNV-1a 64 over the consumed canonical PCM stream
    std::uint32_t stage_count{};
    std::uint32_t events_scheduled{};
    std::uint32_t events_applied{};
    std::uint32_t events_dropped{};
    std::size_t arena_bytes_used{};
};

class Engine
{
public:
    Engine() noexcept = default;
    ~Engine() noexcept;
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;
    Engine(Engine&&) = delete;
    Engine& operator=(Engine&&) = delete;

    // Owns placement-constructed stages whose scratch pointers refer into this
    // instance. Moving/copying it would leave pointers into the old arena.
    // reference_rms: RMS of the *unscaled* source over its reference interval.
    // total_frames: source length in frames, or 0 when unknown (live sources).
    // Success, or why the engine is not configured: validate_scenario()'s
    // verdict, invalid_argument for a reference RMS that is not positive and
    // finite, buffer_too_small when the arena is full, or the Status of the
    // stage whose prepare() refused. Every message is static text.
    [[nodiscard]] m110::Status configure(const Scenario& scenario, double reference_rms, std::uint64_t total_frames) noexcept;

    // As above, with borrowed stage resources (stage_resources.hpp). A Watterson
    // scenario needs resources.watterson; a second-source scenario needs
    // resources.secondary_source. They must outlive the configured engine.
    // The overload above supplies none; missing required resources fail configure.
    [[nodiscard]] m110::Status configure(const Scenario& scenario, double reference_rms, std::uint64_t total_frames, const StageResources& resources) noexcept;

    // Pass-through configuration (no stages, unity gain); useful when no scenario is present.
    void configure_passthrough() noexcept;

    [[nodiscard]] bool configured() const noexcept { return configured_; }

    // frames <= engine_block_frames; output capacity >= frames + engine_slack_frames.
    // The number of output frames written (differs from `frames` only with sample slips;
    // 0 is a block a slip deleted, never a failure), or why there are none: one of
    // engine_failure's Statuses, or the faulting stage's own. A failure is latched
    // (status()) and every later call returns it until configure(). A stage/clip-rejected
    // block can consume input and overwrite the output buffer; callers must discard that
    // buffer. No output statistics/digest are committed. Non-finite input is rejected
    // before input state changes.
    [[nodiscard]] m110::Result<std::size_t> process(const std::int16_t* input, std::size_t frames, std::int16_t* output, std::size_t output_capacity) noexcept;

    // Production path. Inputs and outputs are normalized [-1, 1), and remain
    // float between the source, static engine, and live controller. Statistics
    // and digests describe the eventual signed PCM24 quantization.
    [[nodiscard]] m110::Result<std::size_t> process(const float* input, std::size_t frames, float* output, std::size_t output_capacity) noexcept;

    [[nodiscard]] m110::Result<std::size_t> process(std::span<const float> input, std::span<float> output) noexcept { return process(input.data(), input.size(), output.data(), output.size()); }

    [[nodiscard]] m110::Result<std::size_t> process(std::span<const std::int16_t> input, std::span<std::int16_t> output) noexcept
    {
        return process(input.data(), input.size(), output.data(), output.size());
    }

    // The latch: success while the engine runs; after a failed process(), the Status it
    // returned (its static message says why), until configure() or configure_passthrough().
    [[nodiscard]] m110::Status status() const noexcept { return fault_; }

    [[nodiscard]] const EngineStats& stats() const noexcept { return stats_; }

    // The scenario passed to configure() must outlive the engine's use of it (no copy is kept).
    [[nodiscard]] const Scenario* scenario() const noexcept { return scenario_; }

    [[nodiscard]] const Impairment* stage(std::uint32_t index) const noexcept { return index < stage_count_ ? stages_[index] : nullptr; }

    [[nodiscard]] double scaled_reference_rms() const noexcept { return scaled_reference_; }

    // The realised parameters of the scenario's Watterson stage; nullptr when it has none.
    [[nodiscard]] const WattersonReport* watterson_report() const noexcept { return watterson_stage_.has_value() ? watterson_stage_->report() : nullptr; }

private:
    void destroy_stages() noexcept;
    // Builds a stage in its typed slot or at the arena's next free offset (advancing
    // used): the stage, or buffer_too_small when the arena is full (or the
    // placement constructor's own refusal).
    [[nodiscard]] PlacedStage build_stage(const Stage& description, std::size_t& used) noexcept;
    // Builds a typed-slot stage (AWGN, Watterson) in its slot: the stage,
    // not_an_arena_stage's refusal for an arena stage, or internal_error when no
    // slot is free (validation keeps one free).
    [[nodiscard]] PlacedStage place_in_slot(const Stage& description) noexcept;
    // Keeps `failure` as the engine's status and returns it.
    [[nodiscard]] m110::Status latch(m110::Status failure) noexcept;
    // Whether process() may take a block: the latched failure, or the refusal of an
    // unconfigured engine or a bad block (latched in turn), or success.
    [[nodiscard]] m110::Status admit(bool buffers, std::size_t frames, std::size_t output_capacity) noexcept;
    // Runs the stages over work_ in order: the frame count after the last one, or the
    // latched failure (a stage's fault, or a block the stage lengthened past capacity).
    [[nodiscard]] m110::Result<std::size_t> run_stages(std::size_t frames, std::size_t capacity) noexcept;
    // The stages' event counts into stats_.
    void sum_stage_events() noexcept;
    const Scenario* scenario_{};
    std::uint32_t stage_count_{};
    Impairment* stages_[max_stages]{};
    alignas(impairment_alignment) unsigned char arena_[impairment_arena_bytes]{};
    // AWGN stages live in typed slots, not in the arena: no cast, no placement new (awgn.hpp).
    std::array<std::optional<AwgnStage>, max_awgn_stages> awgn_stages_{};
    // The Watterson stage's adapter; its bulk state is the borrowed resources_.watterson.
    std::optional<WattersonStage> watterson_stage_{};
    std::optional<FrequencyShiftStage> frequency_shift_stage_{};
    std::optional<SecondSourceStage> second_source_stage_{};
    StageResources resources_{};
    float work_[engine_capacity_frames]{};
    float scratch_[engine_capacity_frames]{};
    double input_scale_{1.0 / 32768.0};
    double source_gain_{1.0};
    double scaled_reference_{};
    std::uint64_t cursor_{};
    bool configured_{};
    bool saturate_{true};
    m110::Status fault_{};
    EngineStats stats_{};
    det::StreamDigest output_digest_{};
    det::StreamDigest source_digest_{};
};

} // namespace signal_lab
