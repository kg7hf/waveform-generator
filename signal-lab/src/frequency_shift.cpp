// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

#include "signal_lab/frequency_shift.hpp"

#include "signal_lab/det_math.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <span>

namespace signal_lab
{
namespace
{

constexpr double phase_turn = 18446744073709551616.0;      // 2^64, one unsigned phase word turn.
constexpr double exact_integer_limit = 9007199254740992.0; // 2^53.
constexpr const char* unconfigured_message = "frequency_shift: process() before a successful prepare()";
constexpr const char* contiguity_message = "frequency_shift: first_input_frame does not follow the previous block";
constexpr const char* parameters_message = "frequency_shift: invalid parameters";
constexpr const char* frame_range_message = "frequency_shift: frame range overflows uint64";

[[nodiscard]] bool within(double value, double low, double high) noexcept
{
    return std::isfinite(value) && value >= low && value <= high;
}

[[nodiscard]] std::uint64_t triangle_cycle_step(const FrequencyShiftParams& params, units::SampleRate rate) noexcept
{
    const double amplitude = params.amplitude.in(units::Hz);
    const double slope = params.slope.hertz_per_second();

    if (rate.hertz() == 0U || !std::isfinite(amplitude) || !std::isfinite(slope) || !(amplitude > 0.0) || !(slope > 0.0))
    {
        return 0U;
    }

    const double cycles_per_frame = slope / (4.0 * amplitude * static_cast<double>(rate.hertz()));
    const double ideal = cycles_per_frame * phase_turn;

    if (!std::isfinite(ideal) || ideal < 0.5 || ideal >= exact_integer_limit)
    {
        return 0U;
    }

    // Restrict the step below 2^53 so every integer phase word is exactly
    // representable as double. At most 2 Q0.64 units of per-step error remain.
    return static_cast<std::uint64_t>(det::round_half_even(ideal));
}

[[nodiscard]] std::uint64_t triangle_start_phase(const FrequencyShiftParams& params) noexcept
{
    const double amplitude = params.amplitude.in(units::Hz);
    const double start = params.start_frequency.in(units::Hz);
    const double fraction = params.direction == TriangleDirection::ascending ? (start + amplitude) / (4.0 * amplitude) : 0.5 + (amplitude - start) / (4.0 * amplitude);
    const double scaled = det::round_half_even(fraction * phase_turn);
    return scaled >= phase_turn ? 0U : static_cast<std::uint64_t>(scaled);
}

[[nodiscard]] units::FrequencyDelta triangle_frequency(const FrequencyShiftParams& params, std::uint64_t phase_word) noexcept
{
    const double amplitude = params.amplitude.in(units::Hz);
    const double phase = static_cast<double>(phase_word) / phase_turn;
    const double hertz = phase < 0.5 ? -amplitude + 4.0 * amplitude * phase : amplitude - 4.0 * amplitude * (phase - 0.5);
    return units::FrequencyDelta::from_hz(hertz);
}

[[nodiscard]] std::int64_t phase_increment(units::FrequencyDelta frequency, units::SampleRate rate) noexcept
{
    const double scaled = frequency.in(units::Hz)*phase_turn / static_cast<double>(rate.hertz());
    return static_cast<std::int64_t>(det::round_half_even(scaled));
}

} // namespace

m110::Status validate_frequency_shift_params(const FrequencyShiftParams& params) noexcept
{
    switch (params.mode)
    {
    case FrequencyShiftMode::offset:
        return within(params.offset.in(units::Hz), -frequency_shift_max_offset.in(units::Hz), frequency_shift_max_offset.in(units::Hz))
                   ? m110::Status::success()
                   : m110::Status{m110::StatusCode::invalid_configuration, "frequency_shift.offset_hz must be finite and in [-150, 150]"};
    case FrequencyShiftMode::sweep:
    {
        const double amplitude = params.amplitude.in(units::Hz);
        const double start = params.start_frequency.in(units::Hz);
        const double slope = params.slope.hertz_per_second();
        const bool valid_direction = params.direction == TriangleDirection::ascending || params.direction == TriangleDirection::descending;

        if (!within(amplitude, std::numeric_limits<double>::min(), frequency_shift_max_amplitude.in(units::Hz)) || !within(start, -amplitude, amplitude) || params.offset.in(units::Hz) != 0.0 ||
            !valid_direction || !std::isfinite(slope) || !(slope > 0.0) || triangle_cycle_step(params, stream_rate) == 0U)
        {
            return m110::Status{m110::StatusCode::invalid_configuration, "frequency_shift: invalid sweep amplitude, start, direction or representable positive slope"};
        }

        return m110::Status::success();
    }
    }

    return m110::Status{m110::StatusCode::invalid_configuration, "frequency_shift.mode is invalid"};
}

units::FrequencyDelta instantaneous_frequency(const FrequencyShiftParams& params, units::SampleIndex frame, units::SampleRate rate) noexcept
{
    if (params.mode == FrequencyShiftMode::offset)
    {
        return params.offset;
    }

    const std::uint64_t step = triangle_cycle_step(params, rate);

    if (step == 0U)
    {
        return units::FrequencyDelta{};
    }

    const std::uint64_t phase = triangle_start_phase(params) + frame.value() * step;
    return triangle_frequency(params, phase);
}

m110::Status FrequencyShiftStage::prepare(const StageContext& /*context*/) noexcept
{
    mPrepared = false;
    mHaveFrame = false;
    mNextFrame = 0U;
    mPhase = 0U;
    mSweepPhase = 0U;
    mStats = StageStats{};

    if (mParams == nullptr)
    {
        return m110::Status{m110::StatusCode::invalid_configuration, parameters_message};
    }

    if (const m110::Status valid = validate_frequency_shift_params(*mParams); !valid.is_ok())
    {
        return valid;
    }

    mFilter.configure();
    mSweepStep = mParams->mode == FrequencyShiftMode::sweep ? triangle_cycle_step(*mParams, stream_rate) : 0U;
    mSweepStartPhase = mParams->mode == FrequencyShiftMode::sweep ? triangle_start_phase(*mParams) : 0U;
    mSweepPhase = mSweepStartPhase;
    mStats.events_scheduled = 1U;
    mPrepared = true;
    return m110::Status::success();
}

m110::Result<std::size_t> FrequencyShiftStage::process(float* block, std::size_t frames, std::uint64_t first_input_frame, std::size_t capacity) noexcept
{
    return process_impl(block, frames, first_input_frame, capacity, {}, false);
}

m110::Result<std::size_t> FrequencyShiftStage::process_with_gain(float* block, std::size_t frames, std::uint64_t first_input_frame, std::size_t capacity, std::span<const ComplexFloat> gains) noexcept
{
    if (gains.size() != frames)
    {
        return m110::Status{m110::StatusCode::invalid_argument, "frequency_shift: gain count must equal frame count"};
    }

    for (const ComplexFloat gain : gains)
    {
        if (!std::isfinite(gain.real) || !std::isfinite(gain.imag))
        {
            return m110::Status{m110::StatusCode::invalid_argument, "frequency_shift: gain is not finite"};
        }
    }

    return process_impl(block, frames, first_input_frame, capacity, gains, true);
}

m110::Result<std::size_t> FrequencyShiftStage::process_impl(float* block, std::size_t frames, std::uint64_t first_input_frame, std::size_t capacity, std::span<const ComplexFloat> gains,
                                                            bool apply_gain) noexcept
{
    if (!mPrepared)
    {
        return m110::Status{m110::StatusCode::invalid_configuration, unconfigured_message};
    }

    if ((frames != 0U && block == nullptr) || frames > capacity || frames > std::numeric_limits<std::uint64_t>::max() - first_input_frame)
    {
        return m110::Status{m110::StatusCode::invalid_argument, frame_range_message};
    }

    if (mHaveFrame && first_input_frame != mNextFrame)
    {
        return m110::Status{m110::StatusCode::invalid_argument, contiguity_message};
    }

    if (!mHaveFrame && mParams->mode == FrequencyShiftMode::sweep)
    {
        mSweepPhase = mSweepStartPhase + first_input_frame * mSweepStep;
    }

    const std::size_t chunk_limit = static_cast<std::size_t>(analytic_chunk_frames.value());

    for (std::size_t offset = 0U; offset < frames; offset += chunk_limit)
    {
        const std::size_t count = std::min(chunk_limit, frames - offset);
        const std::uint64_t absolute_first = first_input_frame + offset;
        const std::span<const float> input{block + offset, count};
        const std::span<AnalyticSample> analytic{mAnalytic.data(), count};
        mFilter.process(input, units::SampleIndex{absolute_first}, analytic);

        for (std::size_t index = 0U; index < count; ++index)
        {
            const units::FrequencyDelta here = mParams->mode == FrequencyShiftMode::sweep ? triangle_frequency(*mParams, mSweepPhase) : mParams->offset;
            const units::FrequencyDelta next = mParams->mode == FrequencyShiftMode::sweep ? triangle_frequency(*mParams, mSweepPhase + mSweepStep) : mParams->offset;
            const units::FrequencyDelta interval_frequency = 0.5 * (here + next);
            const double angle = det::two_pi * (static_cast<double>(mPhase) / phase_turn);
            const float cosine = static_cast<float>(det::cos(angle));
            const float sine = static_cast<float>(det::sin(angle));
            const AnalyticSample sample = analytic[index];
            const float shifted = sample.in_phase * cosine - sample.quadrature * sine;

            if (apply_gain)
            {
                const float shifted_quadrature = sample.quadrature * cosine + sample.in_phase * sine;
                block[offset + index] = gains[offset + index].real * shifted - gains[offset + index].imag * shifted_quadrature;
            }
            else
            {
                block[offset + index] = shifted;
            }

            const float output = block[offset + index];
            const float magnitude = std::abs(output);
            mStats.component_peak = std::max(mStats.component_peak, magnitude);
            mStats.component_energy += static_cast<double>(output) * static_cast<double>(output);
            ++mStats.component_frames;
            // Unsigned addition intentionally wraps modulo one 64-bit turn. The
            // interval average integrates a linear frequency segment exactly and
            // keeps phase continuous when the triangle changes slope.
            mPhase += static_cast<std::uint64_t>(phase_increment(interval_frequency, stream_rate));
            mSweepPhase += mSweepStep;
        }
    }

    if (frames != 0U)
    {
        mHaveFrame = true;
        mNextFrame = first_input_frame + frames;
        mStats.events_applied = 1U;
    }

    return frames;
}

} // namespace signal_lab
