#include "signal_lab/mixer.hpp"

namespace signal_lab
{

std::size_t impairment_size(StageType type) noexcept
{
    switch (type)
    {
    case StageType::awgn:
        return awgn_size();
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

Impairment* construct_impairment(const Stage& stage, void* storage, std::size_t storage_bytes) noexcept
{
    switch (stage.type)
    {
    case StageType::awgn:
        return construct_awgn(stage.awgn, storage, storage_bytes);
    case StageType::cw:
        return construct_cw(stage.cw, storage, storage_bytes);
    case StageType::impulse:
        return construct_impulse(stage.impulse, storage, storage_bytes);
    case StageType::fade:
        return construct_fade(stage.fade, storage, storage_bytes);
    case StageType::sample_slip:
        return construct_sample_slip(stage.slip, storage, storage_bytes);
    }
    return nullptr;
}

namespace
{
Scenario passthrough_scenario{};
} // namespace

void Engine::configure_passthrough() noexcept
{
    passthrough_scenario = Scenario{};
    passthrough_scenario.source_gain_db = 0.0;
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
    process_error_ = ProcessError::none;
    stats_ = EngineStats{};
    output_digest_.reset();
    source_digest_.reset();
    configured_ = true;
}

bool Engine::configure(const Scenario& scenario, double reference_rms, std::uint64_t total_frames, const char** error) noexcept
{
    configured_ = false;
    process_error_ = ProcessError::none;
    scenario_ = &scenario;
    stage_count_ = 0U;
    for (auto& stage : stages_)
    {
        stage = nullptr;
    }
    if (!(reference_rms > 0.0))
    {
        if (error != nullptr)
        {
            *error = "reference RMS must be positive";
        }
        return false;
    }
    const double gain = det::db_to_amplitude(scenario.source_gain_db);
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
        const std::size_t needed = impairment_size(description.type);
        if (needed == 0U || used + needed > impairment_arena_bytes)
        {
            if (error != nullptr)
            {
                *error = "impairment arena exhausted";
            }
            return false;
        }
        Impairment* stage = construct_impairment(description, arena_ + used, impairment_arena_bytes - used);
        if (stage == nullptr)
        {
            if (error != nullptr)
            {
                *error = "impairment construction failed";
            }
            return false;
        }
        used += needed;
        const StageContext context{scaled_reference_, total_frames, scenario.seed, index, scratch_};
        if (!stage->prepare(context, error))
        {
            return false;
        }
        stages_[index] = stage;
        stage_count_ = index + 1U;
        stats_.stage_count = stage_count_;
    }
    stats_.arena_bytes_used = used;
    configured_ = true;
    return true;
}

std::size_t Engine::process(const std::int16_t* input, std::size_t frames, std::int16_t* output, std::size_t output_capacity) noexcept
{
    if (process_error_ != ProcessError::none)
    {
        return 0U;
    }
    if (!configured_ || input == nullptr || output == nullptr || frames > engine_block_frames || output_capacity < frames + engine_slack_frames)
    {
        process_error_ = ProcessError::invalid_buffer;
        return 0U;
    }
    source_digest_.update(input, frames);
    stats_.frames_in += frames;
    stats_.source_digest = source_digest_.value();
    for (std::size_t index = 0U; index < frames; ++index)
    {
        work_[index] = static_cast<float>(static_cast<double>(input[index]) * input_scale_);
    }
    std::size_t count = frames;
    std::size_t capacity = output_capacity < engine_capacity_frames ? output_capacity : engine_capacity_frames;
    for (std::uint32_t index = 0U; index < stage_count_; ++index)
    {
        count = stages_[index]->process(work_, count, cursor_, capacity);
    }
    float block_peak = 0.0f;
    double block_energy = 0.0;
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
            if (!saturate_)
            {
                process_error_ = ProcessError::clipping;
            }
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
    stats_.events_scheduled = 0U;
    stats_.events_applied = 0U;
    stats_.events_dropped = 0U;
    for (std::uint32_t index = 0U; index < stage_count_; ++index)
    {
        const StageStats& stage = stages_[index]->stats();
        stats_.events_scheduled += stage.events_scheduled;
        stats_.events_applied += stage.events_applied;
        stats_.events_dropped += stage.events_dropped;
    }
    cursor_ += frames;
    if (process_error_ != ProcessError::none)
    {
        return 0U;
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

std::size_t Engine::process(const float* input, std::size_t frames, float* output,
                            std::size_t output_capacity) noexcept
{
    if (process_error_ != ProcessError::none)
    {
        return 0U;
    }
    if (!configured_ || input == nullptr || output == nullptr ||
        frames > engine_block_frames || output_capacity < frames + engine_slack_frames)
    {
        process_error_ = ProcessError::invalid_buffer;
        return 0U;
    }
    source_digest_.update(input, frames);
    stats_.frames_in += frames;
    stats_.source_digest = source_digest_.value();
    for (std::size_t index = 0U; index < frames; ++index)
    {
        work_[index] = static_cast<float>(static_cast<double>(input[index]) * source_gain_);
    }
    std::size_t count = frames;
    const std::size_t capacity = output_capacity < engine_capacity_frames
                                     ? output_capacity
                                     : engine_capacity_frames;
    for (std::uint32_t index = 0U; index < stage_count_; ++index)
    {
        count = stages_[index]->process(work_, count, cursor_, capacity);
    }

    constexpr double minimum = -1.0;
    constexpr double maximum = static_cast<double>(waveform_generator::audio::pcm24_max) /
                               waveform_generator::audio::pcm24_scale;
    float block_peak = 0.0F;
    double block_energy = 0.0;
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
            if (!saturate_)
            {
                process_error_ = ProcessError::clipping;
            }
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
        const double emitted = static_cast<double>(det::quantize_pcm24(output[index])) /
                               waveform_generator::audio::pcm24_scale;
        const float magnitude = static_cast<float>(emitted < 0.0 ? -emitted : emitted);
        if (magnitude > block_peak)
        {
            block_peak = magnitude;
        }
        block_energy += emitted * emitted;
    }
    stats_.events_scheduled = 0U;
    stats_.events_applied = 0U;
    stats_.events_dropped = 0U;
    for (std::uint32_t index = 0U; index < stage_count_; ++index)
    {
        const StageStats& stage = stages_[index]->stats();
        stats_.events_scheduled += stage.events_scheduled;
        stats_.events_applied += stage.events_applied;
        stats_.events_dropped += stage.events_dropped;
    }
    cursor_ += frames;
    if (process_error_ != ProcessError::none)
    {
        return 0U;
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
