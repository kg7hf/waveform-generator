// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

#include "signal_lab/second_source.hpp"

#include "signal_lab/det_math.hpp"
#include "signal_lab/units/conversions.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <span>

namespace signal_lab
{
namespace
{

constexpr const char* params_message = "second_source: invalid parameters";
constexpr const char* source_message = "second_source: provider is missing or invalid";
constexpr const char* gain_message = "second_source: resolved amplitude is not finite";
constexpr const char* frame_message = "second_source: block must start at frame zero and remain contiguous";
constexpr const char* block_message = "second_source: invalid block or frame range";
constexpr const char* read_size_message = "second_source: provider read exceeds the block limit";
constexpr const char* output_message = "second_source: scaled component is not a finite float";
constexpr const char* fading_message = "second_source.fading.spread_hz must be finite and in [0.1, 10] Hz";
constexpr const char* markov_message = "second_source.markov_on_off means must be finite, at least one frame, and no more than 1e6 seconds";

[[nodiscard]] m110::Status invalid(const char* message) noexcept
{
    return m110::Status{m110::StatusCode::invalid_configuration, message};
}

[[nodiscard]] m110::Status invalid_block(const char* message) noexcept
{
    return m110::Status{m110::StatusCode::invalid_argument, message};
}

[[nodiscard]] std::size_t overlap(std::uint64_t first, std::uint64_t end, std::uint64_t active_first, std::uint64_t active_end, std::uint64_t& overlap_first) noexcept
{
    overlap_first = std::max(first, active_first);
    const std::uint64_t overlap_end = std::min(end, active_end);
    return overlap_end > overlap_first ? static_cast<std::size_t>(overlap_end - overlap_first) : 0U;
}

} // namespace

m110::Status FixedSecondarySource::read(std::uint64_t first, std::span<float> output) noexcept
{
    if (output.size() > engine_block_frames || first > mSamples.size() || output.size() > mSamples.size() - first)
    {
        return invalid_block("second_source: fixed source read is out of bounds");
    }

    std::ranges::copy(mSamples.subspan(static_cast<std::size_t>(first), output.size()), output.begin());
    return m110::Status::success();
}

m110::Status validate_second_source_params(const SecondSourceParams& params) noexcept
{
    const double si_db = params.si.in(units::dB);
    const double offset_hz = params.frequency_offset.in(units::Hz);

    if (!std::isfinite(si_db) || si_db < -120.0 || si_db > 120.0 || params.start_frame > max_second_source_frames || params.source_start_frame > max_second_source_frames ||
        params.duration_frames > max_second_source_frames ||
        (params.duration_frames != 0U && (params.start_frame > max_second_source_frames - params.duration_frames || params.source_start_frame > max_second_source_frames - params.duration_frames)) ||
        !std::isfinite(offset_hz) || std::abs(offset_hz) > second_source_frequency_offset_limit.in(units::Hz))
    {
        return invalid(params_message);
    }

    if (const m110::Status fading = validate_second_source_fading_params(params.fading); !fading.is_ok())
    {
        return fading;
    }

    if (!validate_markov_gate_params(params.markov).is_ok())
    {
        return invalid(markov_message);
    }

    return m110::Status::success();
}

m110::Status validate_second_source_fading_params(const SecondSourceFadingParams& params) noexcept
{
    if (!params.enabled)
    {
        return m110::Status::success();
    }

    const double spread_hz = params.spread.in(units::Hz);

    if (!std::isfinite(spread_hz) || spread_hz < watterson_min_spread.in(units::Hz) || spread_hz > watterson_max_spread.in(units::Hz))
    {
        return invalid(fading_message);
    }

    return m110::Status::success();
}

m110::Status SecondSourceStage::prepare(const StageContext& context) noexcept
{
    mPrepared = false;
    mHaveFrame = false;
    mNextFrame = 0U;
    mActiveFrames = 0U;
    mActiveEnd = 0U;
    mSourceFrames = 0U;
    mScale = 0.0;
    mFadingNextFrame = 0U;
    mShifter.reset();
    mMarkovEnabled = false;
    mStats = StageStats{};
    mFaultStatus = m110::Status::success();

    if (const m110::Status source = prepare_source(context); !source.is_ok())
    {
        return source;
    }

    if (mParams->markov.enabled)
    {
        const StageRandomKey key = context.random_key();

        if (const m110::Status configured = mMarkovGate.configure(mParams->markov, key.seed, key.index); !configured.is_ok())
        {
            return invalid(markov_message);
        }

        mMarkovEnabled = true;
    }

    if (mParams->frequency_offset.in(units::Hz) != 0.0 || mParams->fading.enabled)
    {
        mShiftParams = FrequencyShiftParams{};
        mShiftParams.offset = mParams->frequency_offset;
        mShifter.emplace(mShiftParams);

        if (const m110::Status shifted = mShifter->prepare(context); !shifted.is_ok())
        {
            mShifter.reset();
            return shifted;
        }
    }

    if (mParams->fading.enabled)
    {
        if (const m110::Status faded = prepare_fading(context); !faded.is_ok())
        {
            mShifter.reset();
            return faded;
        }
    }

    mStats.events_scheduled = 1U;
    mPrepared = true;
    return m110::Status::success();
}

m110::Status SecondSourceStage::prepare_fading(const StageContext& context) noexcept
{
    const StageRandomKey key = context.random_key();
    mFadingPath = WattersonPath{};
    mFadingPath.fading = PathFading::rayleigh;
    mFadingPath.spread = mParams->fading.spread;
    mFadingReport = WattersonPathReport{};
    const WattersonPathState::Setup setup{.path = &mFadingPath, .power = 1.0, .index = 0U, .generator = det::Pcg32{key.seed, second_source_fading_rng_family, key.index}, .observer = nullptr};
    return mFadingState.configure_rayleigh(setup, mFadingReport);
}

m110::Status SecondSourceStage::prepare_source(const StageContext& context) noexcept
{
    if (mParams == nullptr || mSource == nullptr || context.scratch == nullptr || !std::isfinite(context.reference_rms) || !(context.reference_rms > 0.0))
    {
        return invalid(source_message);
    }

    if (const m110::Status valid = validate_second_source_params(*mParams); !valid.is_ok())
    {
        return valid;
    }

    mSourceFrames = mSource->frames();
    const double source_rms = mSource->reference_rms();

    if (mSourceFrames == 0U || !std::isfinite(source_rms) || !(source_rms > 0.0) || mParams->source_start_frame >= mSourceFrames)
    {
        return invalid(source_message);
    }

    const std::uint64_t available = mSourceFrames - mParams->source_start_frame;
    mActiveFrames = mParams->duration_frames == 0U ? available : mParams->duration_frames;

    if (mActiveFrames > available || mActiveFrames > max_second_source_frames || mParams->start_frame > max_second_source_frames - mActiveFrames ||
        mParams->source_start_frame > max_second_source_frames - mActiveFrames)
    {
        return invalid("second_source: requested source interval is outside bounds");
    }

    mActiveEnd = mParams->start_frame + mActiveFrames;
    const double field_ratio = units::amplitude_ratio(units::Ratio::from_db(-mParams->si.in(units::dB)));
    mScale = (context.reference_rms / source_rms) * field_ratio;

    if (!std::isfinite(mScale) || !(mScale > 0.0))
    {
        return invalid(gain_message);
    }

    mScratch = context.scratch;
    return m110::Status::success();
}

m110::Status SecondSourceStage::read_active(std::uint64_t first, std::span<float> output) noexcept
{
    if (output.size() > engine_block_frames)
    {
        return invalid_block(read_size_message);
    }

    std::ranges::fill(output, 0.0F);
    const std::uint64_t block_end = first + output.size();
    std::uint64_t active_first = 0U;
    const std::size_t count = overlap(first, block_end, mParams->start_frame, mActiveEnd, active_first);

    if (count == 0U)
    {
        return m110::Status::success();
    }

    const std::uint64_t source_frame = mParams->source_start_frame + (active_first - mParams->start_frame);
    const std::size_t output_offset = static_cast<std::size_t>(active_first - first);
    const m110::Status read = mSource->read(source_frame, output.subspan(output_offset, count));

    if (!read.is_ok())
    {
        return read;
    }

    for (const float sample : output.subspan(output_offset, count))
    {
        if (!std::isfinite(sample))
        {
            return invalid("second_source: provider returned a non-finite sample");
        }
    }

    return m110::Status::success();
}

m110::Status SecondSourceStage::process_offset(FrequencyShiftStage& shifter, float* block, std::size_t frames, std::uint64_t first_input_frame) noexcept
{
    std::span<float> scratch{mScratch, frames};
    const m110::Status read = read_active(first_input_frame, scratch);

    if (!read.is_ok())
    {
        return read;
    }

    std::uint64_t active_first = 0U;
    const std::size_t active_count = overlap(first_input_frame, first_input_frame + frames, mParams->start_frame, mActiveEnd, active_first);

    if (active_count != 0U)
    {
        const std::size_t active_offset = static_cast<std::size_t>(active_first - first_input_frame);
        const std::uint64_t local_first = active_first - mParams->start_frame;

        if (mParams->fading.enabled)
        {
            if (const m110::Status faded = process_active_shifter(shifter, mScratch + active_offset, local_first, active_count); !faded.is_ok())
            {
                return faded;
            }
        }
        else
        {
            const m110::Result<std::size_t> shifted = shifter.process(mScratch + active_offset, active_count, local_first, active_count);

            if (!shifted.has_value())
            {
                return shifted.status();
            }
        }
    }

    return add_component(block, std::span<const float>{mScratch, frames}, first_input_frame);
}

m110::Status SecondSourceStage::process_active_shifter(FrequencyShiftStage& shifter, float* samples, std::uint64_t local_first, std::size_t count) noexcept
{
    if (local_first != mFadingNextFrame || count > max_second_source_frames - local_first)
    {
        return invalid_block(frame_message);
    }

    const std::size_t chunk_limit = static_cast<std::size_t>(analytic_chunk_frames.value());

    for (std::size_t offset = 0U; offset < count; offset += chunk_limit)
    {
        const std::size_t chunk = std::min(chunk_limit, count - offset);

        for (std::size_t index = 0U; index < chunk; ++index)
        {
            mGainChunk[index] = mFadingState.next_gain(units::SampleIndex{local_first + offset + index});
        }

        const m110::Result<std::size_t> processed = shifter.process_with_gain(samples + offset, chunk, local_first + offset, chunk, std::span<const ComplexFloat>{mGainChunk.data(), chunk});

        if (!processed.has_value())
        {
            return processed.status();
        }

        mFadingNextFrame += chunk;
    }

    return m110::Status::success();
}

m110::Status SecondSourceStage::process_unshifted(float* block, std::size_t frames, std::uint64_t first_input_frame) noexcept
{
    std::span<float> scratch{mScratch, frames};
    const m110::Status read = read_active(first_input_frame, scratch);

    if (!read.is_ok())
    {
        return read;
    }

    return add_component(block, scratch, first_input_frame);
}

m110::Status SecondSourceStage::add_component(float* block, std::span<const float> component, std::uint64_t first_input_frame) noexcept
{
    const double float_limit = static_cast<double>(std::numeric_limits<float>::max());

    for (std::size_t index = 0U; index < component.size(); ++index)
    {
        const double scaled = static_cast<double>(component[index]) * mScale;

        if (!std::isfinite(scaled) || std::abs(scaled) > float_limit || !std::isfinite(block[index]))
        {
            return invalid(output_message);
        }

        const float rounded_component = static_cast<float>(scaled);
        const double sum = static_cast<double>(block[index]) + static_cast<double>(rounded_component);

        if (std::abs(sum) > float_limit)
        {
            return invalid(output_message);
        }
    }

    std::uint64_t active_first = 0U;
    const std::size_t count = overlap(first_input_frame, first_input_frame + component.size(), mParams->start_frame, mActiveEnd, active_first);
    const std::size_t offset = static_cast<std::size_t>(active_first - first_input_frame);

    for (std::size_t index = 0U; index < component.size(); ++index)
    {
        const float value = static_cast<float>(static_cast<double>(component[index]) * mScale);
        const bool active = index >= offset && index - offset < count;

        if (!active)
        {
            block[index] += value;
            continue;
        }

        const bool gate_on = !mMarkovEnabled || mMarkovGate.on();

        if (gate_on)
        {
            block[index] += value;
            mStats.component_peak = std::max(mStats.component_peak, std::abs(value));
            mStats.component_energy += static_cast<double>(value) * static_cast<double>(value);
        }

        ++mStats.component_frames;

        if (mMarkovEnabled)
        {
            static_cast<void>(mMarkovGate.advance());
        }
    }

    if (count != 0U)
    {
        mStats.events_applied = 1U;
    }

    return m110::Status::success();
}

m110::Result<std::size_t> SecondSourceStage::process(float* block, std::size_t frames, std::uint64_t first_input_frame, std::size_t capacity) noexcept
{
    if (!mFaultStatus.is_ok())
    {
        return mFaultStatus;
    }

    if (!mPrepared)
    {
        return invalid("second_source: process() before successful prepare()");
    }

    if (frames == 0U)
    {
        return 0U;
    }

    if (block == nullptr || frames > capacity || frames > engine_block_frames || frames > std::numeric_limits<std::uint64_t>::max() - first_input_frame)
    {
        return invalid_block(block_message);
    }

    if ((mHaveFrame && first_input_frame != mNextFrame) || (!mHaveFrame && first_input_frame != 0U))
    {
        return invalid_block(frame_message);
    }

    const m110::Status processed = mShifter.has_value() ? process_offset(*mShifter, block, frames, first_input_frame) : process_unshifted(block, frames, first_input_frame);

    if (!processed.is_ok())
    {
        mFaultStatus = processed;
        return mFaultStatus;
    }

    mHaveFrame = true;
    mNextFrame = first_input_frame + frames;
    return frames;
}

} // namespace signal_lab
