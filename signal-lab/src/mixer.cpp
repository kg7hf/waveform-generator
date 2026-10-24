// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// Signal pipeline
// Guide: docs/modules/signal-lab-engine.md
// Ownership: Borrows an immutable scenario; owns the placement arena and scratch.
// Contract: configure validates geometry; process latches a failure as a Status and callers discard rejected output.

#include "signal_lab/mixer.hpp"

#include <cmath>
#include <iterator>

namespace signal_lab
{

std::size_t impairment_size(StageType type) noexcept
{
    switch (type)
    {
    case StageType::awgn:
    case StageType::watterson:
    case StageType::frequency_shift:
    case StageType::second_source:
        return 0U; // typed engine slots, not the arena (awgn.hpp, watterson.hpp)

    case StageType::cw:
        return cw_size();

    case StageType::impulse:
        return impulse_size();

    case StageType::fade:
        return fade_size();

    case StageType::sample_slip:
        return sample_slip_size();
    }

    return 0U;
}

PlacedStage construct_impairment(const Stage& stage, void* storage, std::size_t storage_bytes) noexcept
{
    switch (stage.type)
    {
    case StageType::awgn:
    case StageType::watterson:
    case StageType::frequency_shift:
    case StageType::second_source:
        return not_an_arena_stage; // typed engine slots, not the arena (awgn.hpp, watterson.hpp)

    case StageType::cw:
        return construct_cw(stage.cw, storage, storage_bytes);

    case StageType::impulse:
        return construct_impulse(stage.impulse, storage, storage_bytes);

    case StageType::fade:
        return construct_fade(stage.fade, storage, storage_bytes);

    case StageType::sample_slip:
        return construct_sample_slip(stage.slip, storage, storage_bytes);
    }

    return not_an_arena_stage;
}

namespace
{
constexpr Scenario passthrough_scenario = []
{
    Scenario result{};
    result.source_gain_db = 0.0;
    return result;
}();

constexpr m110::Status arena_exhausted{.code = m110::StatusCode::buffer_too_small, .message = "impairment arena exhausted"};
// Validation allows at most max_awgn_stages and one Watterson stage, so a slot is free.
constexpr m110::Status no_free_slot{.code = m110::StatusCode::internal_error, .message = "impairment construction failed: no free typed slot"};
constexpr m110::Status reference_not_positive{.code = m110::StatusCode::invalid_argument, .message = "reference RMS must be positive"};
constexpr m110::Status scaled_reference_not_finite{.code = m110::StatusCode::invalid_argument, .message = "scaled reference RMS is not finite"};

// The stages the engine builds in typed slots instead of the placement arena.
[[nodiscard]] constexpr bool built_in_slot(StageType type) noexcept
{
    return type == StageType::awgn || type == StageType::watterson || type == StageType::frequency_shift || type == StageType::second_source;
}

// Ends an arena stage's lifetime; a typed slot's stage ends when its slot is reset.
void discard_stage(Impairment& stage) noexcept
{
    if (!built_in_slot(stage.type()))
    {
        stage.~Impairment();
    }
}
} // namespace

void Engine::configure_passthrough() noexcept
{
    destroy_stages();
    scenario_ = &passthrough_scenario;
    stage_count_ = 0U;

    for (auto& stage : stages_)
    {
        stage = nullptr;
    }

    input_scale_ = 1.0 / 32768.0;
    source_gain_ = 1.0;
    scaled_reference_ = 0.0;
    cursor_ = 0U;
    saturate_ = true;
    fault_ = m110::Status::success();
    stats_ = EngineStats{};
    output_digest_.reset();
    source_digest_.reset();
    configured_ = true;
}

Engine::~Engine() noexcept
{
    destroy_stages();
}

void Engine::destroy_stages() noexcept
{
    for (Impairment*& stage : std::span{stages_}.first(stage_count_))
    {
        discard_stage(*stage);
        stage = nullptr;
    }

    for (std::optional<AwgnStage>& slot : awgn_stages_)
    {
        slot.reset();
    }

    watterson_stage_.reset();
    frequency_shift_stage_.reset();
    second_source_stage_.reset();
    stage_count_ = 0U;
}

PlacedStage Engine::build_stage(const Stage& description, std::size_t& used) noexcept
{
    if (built_in_slot(description.type))
    {
        return place_in_slot(description);
    }

    const std::size_t needed = impairment_size(description.type);

    if (needed == 0U || used + needed > impairment_arena_bytes)
    {
        return arena_exhausted;
    }

    const std::span<unsigned char> free_space = std::span{arena_}.subspan(used);
    const PlacedStage placed = construct_impairment(description, free_space.data(), free_space.size());
    used += placed.has_value() ? needed : 0U;
    return placed;
}

PlacedStage Engine::place_in_slot(const Stage& description) noexcept
{
    if (description.type == StageType::second_source)
    {
        return std::ref<Impairment>(second_source_stage_.emplace(scenario_->second_source, resources_.secondary_source));
    }

    if (description.type == StageType::frequency_shift)
    {
        return std::ref<Impairment>(frequency_shift_stage_.emplace(scenario_->frequency_shift));
    }

    if (description.type == StageType::watterson)
    {
        // Validation allows one Watterson stage; its parameters are the scenario's slot.
        return std::ref<Impairment>(watterson_stage_.emplace(scenario_->watterson, resources_.watterson, resources_.tap_observer));
    }

    if (description.type != StageType::awgn)
    {
        return not_an_arena_stage;
    }

    for (std::optional<AwgnStage>& slot : awgn_stages_)
    {
        if (!slot.has_value())
        {
            return std::ref<Impairment>(slot.emplace(description.awgn));
        }
    }

    return no_free_slot;
}

m110::Status Engine::configure(const Scenario& scenario, double reference_rms, std::uint64_t total_frames) noexcept
{
    return configure(scenario, reference_rms, total_frames, StageResources{});
}

m110::Status Engine::configure(const Scenario& scenario, double reference_rms, std::uint64_t total_frames, const StageResources& resources) noexcept
{
    destroy_stages();
    resources_ = resources;
    configured_ = false;
    fault_ = m110::Status::success();
    scenario_ = &scenario;
    stage_count_ = 0U;

    for (auto& stage : stages_)
    {
        stage = nullptr;
    }

    if (const m110::Status valid = validate_scenario(scenario); !valid.is_ok())
    {
        return valid;
    }

    if (!(reference_rms > 0.0) || !std::isfinite(reference_rms))
    {
        return reference_not_positive;
    }

    const double gain = det::db_to_amplitude(scenario.source_gain_db);

    if (!std::isfinite(reference_rms * gain))
    {
        return scaled_reference_not_finite;
    }

    input_scale_ = gain / 32768.0;
    source_gain_ = gain;
    scaled_reference_ = reference_rms * gain;
    saturate_ = scenario.saturate;
    cursor_ = 0U;
    stats_ = EngineStats{};
    output_digest_.reset();
    source_digest_.reset();
    std::size_t used = 0U;

    for (std::uint32_t index = 0U; index < scenario.stage_count; ++index)
    {
        const Stage& description = scenario.stages[index];
        const PlacedStage built = build_stage(description, used);

        if (!built.has_value())
        {
            return built.error();
        }

        Impairment& stage = built.value();
        const StageContext context{scaled_reference_, total_frames, scenario.seed, index, scratch_, scenario.noise_bandwidth, description.stream_id};

        if (const m110::Status prepared = stage.prepare(context); !prepared.is_ok())
        {
            discard_stage(stage);
            destroy_stages();
            return prepared;
        }

        std::span{stages_}[index] = &stage;
        stage_count_ = index + 1U;
        stats_.stage_count = stage_count_;
    }

    stats_.arena_bytes_used = used;
    configured_ = true;
    return m110::Status::success();
}

m110::Status Engine::latch(m110::Status failure) noexcept
{
    fault_ = failure;
    return failure;
}

m110::Status Engine::admit(bool buffers, std::size_t frames, std::size_t output_capacity) noexcept
{
    if (!fault_.is_ok())
    {
        return fault_;
    }

    if (!configured_)
    {
        return latch(engine_failure::not_configured);
    }

    if (!buffers || frames > engine_block_frames || output_capacity < frames + engine_slack_frames)
    {
        return latch(engine_failure::invalid_block);
    }

    return m110::Status::success();
}

m110::Result<std::size_t> Engine::run_stages(std::size_t frames, std::size_t capacity) noexcept
{
    std::size_t count = frames;

    for (Impairment* const stage : std::span{stages_}.first(stage_count_))
    {
        const m110::Result<std::size_t> processed = stage->process(std::data(work_), count, cursor_, capacity);

        if (!processed.has_value())
        {
            return latch(processed.error());
        }

        count = processed.value();

        // A stage may lengthen a block only into the slack (sample slips); more is an
        // engine invariant broken by the stage, not a reporting channel.
        if (count > capacity)
        {
            return latch(engine_failure::block_overrun);
        }
    }

    return count;
}

void Engine::sum_stage_events() noexcept
{
    stats_.events_scheduled = 0U;
    stats_.events_applied = 0U;
    stats_.events_dropped = 0U;

    for (const Impairment* const stage : std::span{stages_}.first(stage_count_))
    {
        const StageStats& counts = stage->stats();
        stats_.events_scheduled += counts.events_scheduled;
        stats_.events_applied += counts.events_applied;
        stats_.events_dropped += counts.events_dropped;
    }
}

m110::Result<std::size_t> Engine::process(const std::int16_t* input, std::size_t frames, std::int16_t* output, std::size_t output_capacity) noexcept
{
    if (const m110::Status admitted = admit(input != nullptr && output != nullptr, frames, output_capacity); !admitted.is_ok())
    {
        return admitted;
    }

    source_digest_.update(input, frames);
    stats_.frames_in += frames;
    stats_.source_digest = source_digest_.value();

    for (std::size_t index = 0U; index < frames; ++index)
    {
        work_[index] = static_cast<float>(static_cast<double>(input[index]) * input_scale_);
    }

    const std::size_t capacity = output_capacity < engine_capacity_frames ? output_capacity : engine_capacity_frames;
    const m110::Result<std::size_t> staged = run_stages(frames, capacity);

    if (!staged.has_value())
    {
        return staged;
    }

    const std::size_t count = staged.value();

    for (const float value : std::span{work_, count})
    {
        if (!std::isfinite(value))
        {
            return latch(engine_failure::nonfinite_output);
        }
    }

    float block_peak = 0.0f;
    double block_energy = 0.0;
    bool rejected = false;

    for (std::size_t index = 0U; index < count; ++index)
    {
        const float sample = work_[index];
        const double scaled = static_cast<double>(sample) * 32768.0;
        double rounded = det::round_half_even(scaled);

        if (scaled < -32768.0 || scaled > 32767.0)
        {
            stats_.clipped_samples++;

            if (stats_.first_clipped_frame == 0xFFFFFFFFFFFFFFFFULL)
            {
                stats_.first_clipped_frame = stats_.frames_out + index;
            }

            rejected = rejected || !saturate_;
        }

        if (rounded < -32768.0)
        {
            rounded = -32768.0;
        }
        else if (rounded > 32767.0)
        {
            rounded = 32767.0;
        }

        output[index] = static_cast<std::int16_t>(rounded);
        const double emitted = static_cast<double>(output[index]) / 32768.0;
        const float magnitude = static_cast<float>(emitted < 0.0 ? -emitted : emitted);

        if (magnitude > block_peak)
        {
            block_peak = magnitude;
        }

        block_energy += emitted * emitted;
    }

    sum_stage_events();
    cursor_ += frames;

    if (rejected)
    {
        return latch(engine_failure::clipping);
    }

    if (block_peak > stats_.peak)
    {
        stats_.peak = block_peak;
    }

    stats_.output_energy += block_energy;
    output_digest_.update(output, count);
    stats_.frames_out += count;
    stats_.output_digest = output_digest_.value();
    return count;
}

m110::Result<std::size_t> Engine::process(const float* input, std::size_t frames, float* output, std::size_t output_capacity) noexcept
{
    if (const m110::Status admitted = admit(input != nullptr && output != nullptr, frames, output_capacity); !admitted.is_ok())
    {
        return admitted;
    }

    for (const float value : std::span{input, frames})
    {
        if (!std::isfinite(value))
        {
            return latch(engine_failure::nonfinite_input);
        }
    }

    source_digest_.update(input, frames);
    stats_.frames_in += frames;
    stats_.source_digest = source_digest_.value();

    for (std::size_t index = 0U; index < frames; ++index)
    {
        work_[index] = static_cast<float>(static_cast<double>(input[index]) * source_gain_);
    }

    const std::size_t capacity = output_capacity < engine_capacity_frames ? output_capacity : engine_capacity_frames;
    const m110::Result<std::size_t> staged = run_stages(frames, capacity);

    if (!staged.has_value())
    {
        return staged;
    }

    const std::size_t count = staged.value();
    constexpr double minimum = -1.0;
    constexpr double maximum = static_cast<double>(waveform_generator::audio::pcm24_max) / waveform_generator::audio::pcm24_scale;

    for (const float value : std::span{work_, count})
    {
        if (!std::isfinite(value))
        {
            return latch(engine_failure::nonfinite_output);
        }
    }

    float block_peak = 0.0F;
    double block_energy = 0.0;
    bool rejected = false;

    for (std::size_t index = 0U; index < count; ++index)
    {
        double sample = static_cast<double>(work_[index]);

        if (sample < minimum || sample > maximum)
        {
            ++stats_.clipped_samples;

            if (stats_.first_clipped_frame == 0xFFFFFFFFFFFFFFFFULL)
            {
                stats_.first_clipped_frame = stats_.frames_out + index;
            }

            rejected = rejected || !saturate_;
        }

        if (sample < minimum)
        {
            sample = minimum;
        }
        else if (sample > maximum)
        {
            sample = maximum;
        }

        output[index] = static_cast<float>(sample);
        const double emitted = static_cast<double>(det::quantize_pcm24(output[index])) / waveform_generator::audio::pcm24_scale;
        const float magnitude = static_cast<float>(emitted < 0.0 ? -emitted : emitted);

        if (magnitude > block_peak)
        {
            block_peak = magnitude;
        }

        block_energy += emitted * emitted;
    }

    sum_stage_events();
    cursor_ += frames;

    if (rejected)
    {
        return latch(engine_failure::clipping);
    }

    if (block_peak > stats_.peak)
    {
        stats_.peak = block_peak;
    }

    stats_.output_energy += block_energy;
    output_digest_.update(output, count);
    stats_.frames_out += count;
    stats_.output_digest = output_digest_.value();
    return count;
}

} // namespace signal_lab
