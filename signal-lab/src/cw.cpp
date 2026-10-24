// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

#include "cw_pulse_scenario.hpp"
#include "signal_lab/cw_fading.hpp"
#include "signal_lab/cw_fsk.hpp"
#include "signal_lab/det_math.hpp"
#include "signal_lab/impairment.hpp"
#include "signal_lab/markov_gate.hpp"
#include "signal_lab/watterson.hpp"

#include <cmath>
#include <limits>
#include <new>

namespace signal_lab
{

void resolve_window(const Window& window, std::uint64_t total_frames, std::uint64_t& start, std::uint64_t& end) noexcept
{
    start = det::seconds_to_frames(window.start_seconds, sample_rate_hz);

    if (window.has_duration)
    {
        end = start + det::seconds_to_frames(window.duration_seconds, sample_rate_hz);

        if (total_frames != 0U && end > total_frames)
        {
            end = total_frames;
        }
    }
    else
    {
        end = total_frames != 0U ? total_frames : unbounded_frames;
    }

    if (end < start)
    {
        end = start;
    }
}

namespace
{

constexpr m110::Status invalid_sweep_frequency{.code = m110::StatusCode::invalid_configuration, .message = "cw: configure exactly one fixed frequency or frequency sweep"};
constexpr m110::Status sweep_frame_order{.code = m110::StatusCode::invalid_argument, .message = "cw frequency sweep: process must begin at frame zero and remain contiguous"};
constexpr m110::Status sweep_frame_range{.code = m110::StatusCode::invalid_argument, .message = "cw frequency sweep: frame range overflows uint64"};
constexpr m110::Status sweep_unprepared{.code = m110::StatusCode::invalid_configuration, .message = "cw frequency sweep: process() before successful prepare()"};
constexpr m110::Status sweep_geometry{.code = m110::StatusCode::invalid_configuration, .message = "cw gated tone: invalid phase, level, ramp or window"};
constexpr m110::Status markov_pulse_conflict{.code = m110::StatusCode::invalid_configuration, .message = "cw.markov_on_off cannot be combined with periodic pulse timing"};
constexpr m110::Status gated_frame_order{.code = m110::StatusCode::invalid_argument, .message = "cw gate: process must begin at frame zero and remain contiguous"};
constexpr m110::Status gated_frame_range{.code = m110::StatusCode::invalid_argument, .message = "cw gate: invalid capacity or frame range overflow"};
constexpr m110::Status gated_unprepared{.code = m110::StatusCode::invalid_configuration, .message = "cw gate: process() before successful prepare()"};
constexpr m110::Status invalid_fixed_frequency{.code = m110::StatusCode::invalid_configuration, .message = "cw.frequency_hz out of range"};
constexpr m110::Status fsk_sweep_conflict{.code = m110::StatusCode::invalid_configuration, .message = "cw.fsk and cw.frequency_sweep are mutually exclusive"};
constexpr m110::Status fsk_frame_order{.code = m110::StatusCode::invalid_argument, .message = "cw FSK: process must begin at frame zero and remain contiguous"};
constexpr m110::Status fsk_frame_range{.code = m110::StatusCode::invalid_argument, .message = "cw FSK: invalid capacity or frame range overflow"};
constexpr m110::Status fsk_unprepared{.code = m110::StatusCode::invalid_configuration, .message = "cw FSK: process() before successful prepare()"};
constexpr double phase_turn = 18446744073709551616.0;
constexpr double max_sweep_geometry_seconds = 1.0e9;

[[nodiscard]] bool finite_between(double value, double low, double high) noexcept
{
    return std::isfinite(value) && value >= low && value <= high;
}

[[nodiscard]] bool valid_sweep_geometry(const CwParams& params, const StageContext& context) noexcept
{
    return finite_between(params.ci_db, -120.0, 120.0) && finite_between(params.phase_degrees, -360.0, 360.0) && finite_between(params.ramp_seconds, 0.0, max_sweep_geometry_seconds) &&
           finite_between(params.window.start_seconds, 0.0, max_sweep_geometry_seconds) &&
           (!params.window.has_duration || finite_between(params.window.duration_seconds, 0.0, max_sweep_geometry_seconds)) && std::isfinite(context.reference_rms) && context.reference_rms > 0.0;
}

[[nodiscard]] m110::Status validate_gated_configuration(const CwParams& params, const StageContext& context) noexcept
{
    if (params.sweep.enabled && params.fsk.enabled)
    {
        return fsk_sweep_conflict;
    }

    if (params.sweep.enabled && params.frequency_hz != 0.0)
    {
        return invalid_sweep_frequency;
    }

    if (!params.sweep.enabled && (!(params.frequency_hz > 0.0) || params.frequency_hz >= static_cast<double>(sample_rate_hz) / 2.0))
    {
        return invalid_fixed_frequency;
    }

    if (params.markov.enabled && (params.pulse_period_frames != 0U || params.pulse_on_frames != 0U))
    {
        return markov_pulse_conflict;
    }

    if (!valid_sweep_geometry(params, context))
    {
        return sweep_geometry;
    }

    if (params.sweep.enabled)
    {
        if (const m110::Status status = validate_cw_sweep_params(params.sweep); !status.is_ok())
        {
            return status;
        }
    }

    if (params.markov.enabled)
    {
        if (const m110::Status status = validate_markov_gate_params(params.markov); !status.is_ok())
        {
            return status;
        }
    }

    if (params.fsk.enabled)
    {
        if (const m110::Status status = validate_cw_fsk_params(params.fsk, params.frequency_hz); !status.is_ok())
        {
            return status;
        }
    }

    if (params.fading.enabled)
    {
        if (const m110::Status status = validate_cw_fading_params(params.fading); !status.is_ok())
        {
            return status;
        }
    }

    return validate_cw_pulse_timing(params);
}

void count_event(std::uint32_t& count) noexcept
{
    if (count != UINT32_MAX)
    {
        ++count;
    }
}

[[nodiscard]] std::uint32_t scheduled_pulse_count(std::uint64_t length, std::uint64_t period) noexcept
{
    if (length == 0U || length == unbounded_frames)
    {
        return 0U;
    }

    const std::uint64_t count = 1U + (length - 1U) / period;
    return count > UINT32_MAX ? UINT32_MAX : static_cast<std::uint32_t>(count);
}

[[nodiscard]] double pulse_envelope(std::uint64_t offset, std::uint64_t on_length, std::uint64_t ramp) noexcept
{
    const std::uint64_t pulse_ramp = ramp < on_length / 2U ? ramp : on_length / 2U;

    if (pulse_ramp == 0U)
    {
        return 1.0;
    }

    const std::uint64_t tail = on_length - 1U - offset;
    const std::uint64_t distance = tail < offset ? tail : offset;

    if (distance >= pulse_ramp)
    {
        return 1.0;
    }

    const double edge = det::sin(det::half_pi * (static_cast<double>(distance) / static_cast<double>(pulse_ramp)));
    return edge * edge;
}

struct PulseFrame
{
    bool active{};
    bool starts{};
    double envelope{};
};

[[nodiscard]] PulseFrame pulse_frame(std::uint64_t elapsed, std::uint64_t length, std::uint64_t period, std::uint64_t on_frames, std::uint64_t ramp) noexcept
{
    const std::uint64_t pulse_offset = elapsed % period;
    const std::uint64_t pulse_start = elapsed - pulse_offset;
    const std::uint64_t remaining = length == unbounded_frames ? on_frames : length - pulse_start;
    const std::uint64_t on_length = remaining < on_frames ? remaining : on_frames;

    if (pulse_offset >= on_length)
    {
        return PulseFrame{.active = false, .starts = pulse_offset == 0U};
    }

    return PulseFrame{.active = true, .starts = pulse_offset == 0U, .envelope = pulse_envelope(pulse_offset, on_length, ramp)};
}

[[nodiscard]] double legacy_envelope(std::uint64_t elapsed, std::uint64_t length, std::uint64_t ramp) noexcept
{
    double envelope = 1.0;

    if (ramp != 0U)
    {
        std::uint64_t distance = elapsed;

        if (length != unbounded_frames)
        {
            const std::uint64_t tail = length - 1U - elapsed;
            distance = tail < distance ? tail : distance;
        }

        if (distance < ramp)
        {
            const double edge = det::sin(det::half_pi * (static_cast<double>(distance) / static_cast<double>(ramp)));
            envelope = edge * edge;
        }
    }

    return envelope;
}

void record_pulse_start(StageStats& stats, bool unknown_length) noexcept
{
    count_event(stats.events_applied);

    if (unknown_length)
    {
        count_event(stats.events_scheduled);
    }
}

void record_sample(StageStats& stats, float value) noexcept
{
    const float magnitude = value < 0.0F ? -value : value;

    if (magnitude > stats.component_peak)
    {
        stats.component_peak = magnitude;
    }

    stats.component_energy += static_cast<double>(value) * static_cast<double>(value);
    stats.component_frames++;
}

[[nodiscard]] std::uint64_t carrier_phase_word(double phase_radians) noexcept
{
    double turns = phase_radians / det::two_pi;

    if (turns < 0.0)
    {
        turns += 1.0;
    }
    else if (turns >= 1.0)
    {
        turns -= 1.0;
    }

    const double rounded = det::round_half_even(turns * phase_turn);
    return rounded >= phase_turn ? 0U : static_cast<std::uint64_t>(rounded);
}

[[nodiscard]] std::uint64_t carrier_phase_step(double frequency_hz) noexcept
{
    return static_cast<std::uint64_t>(det::round_half_even(frequency_hz * phase_turn / static_cast<double>(sample_rate_hz)));
}

class CwImpairment final : public Impairment
{
public:
    explicit CwImpairment(const CwParams& params) noexcept : params_{params} {}

    [[nodiscard]] StageType type() const noexcept override { return StageType::cw; }

    [[nodiscard]] m110::Status prepare(const StageContext& context) noexcept override
    {
        mSequentialPrepared = false;
        mMarkovEnabled = false;
        mFskEnabled = false;
        mFadingEnabled = false;

        if (requires_gated_validation())
        {
            if (const m110::Status status = validate_gated_configuration(params_, context); !status.is_ok())
            {
                return status;
            }
        }
        else if (!(params_.frequency_hz > 0.0) || params_.frequency_hz >= static_cast<double>(sample_rate_hz) / 2.0)
        {
            return invalid_fixed_frequency;
        }
        else if (const m110::Status timing = validate_cw_pulse_timing(params_); !timing.is_ok())
        {
            return timing;
        }

        mPulsed = params_.pulse_period_frames != 0U;
        resolve_window(params_.window, context.total_frames, start_, end_);
        length_ = end_ == unbounded_frames ? unbounded_frames : end_ - start_;
        ramp_ = det::seconds_to_frames(params_.ramp_seconds, sample_rate_hz);

        if (length_ != unbounded_frames && 2U * ramp_ > length_)
        {
            ramp_ = length_ / 2U;
        }

        const StageRandomKey key = context.random_key();
        phase_radians_ = params_.random_phase ? det::Pcg32(key.seed, 7U, key.index).uniform() * det::two_pi : params_.phase_degrees * (det::pi / 180.0);
        amplitude_ = std::sqrt(2.0) * context.reference_rms * det::db_to_amplitude(-params_.ci_db);

        if (requires_gated_validation() && !std::isfinite(amplitude_))
        {
            return sweep_geometry;
        }

        radians_per_frame_ = det::two_pi * params_.frequency_hz / static_cast<double>(sample_rate_hz);
        mSweepStep = params_.sweep.enabled ? cw_sweep_cycle_step(params_.sweep) : 0U;
        mSweepPhase = 0U;
        mCarrierPhase = (params_.sweep.enabled || params_.fsk.enabled) ? carrier_phase_word(phase_radians_) : 0U;
        mSweepNextFrame = 0U;
        mSweepStarted = false;
        mMarkovEnabled = params_.markov.enabled;
        mFskEnabled = params_.fsk.enabled;

        if (const m110::Status configured = configure_optional_generators(context); !configured.is_ok())
        {
            return configured;
        }

        stats_ = StageStats{};
        stats_.events_scheduled = 1U;

        if (mMarkovEnabled)
        {
            stats_.events_scheduled = 0U;
            stats_.events_applied = 0U;
        }
        else if (mPulsed)
        {
            stats_.events_applied = 0U;
            stats_.events_scheduled = scheduled_pulse_count(length_, params_.pulse_period_frames);
        }

        mSequentialPrepared = sequential_mode();
        return m110::Status::success();
    }

    [[nodiscard]] m110::Result<std::size_t> process(float* block, std::size_t frames, std::uint64_t first_input_frame, std::size_t capacity) noexcept override
    {
        if (params_.sweep.enabled)
        {
            return process_sweep(block, frames, first_input_frame, capacity);
        }

        if (params_.fsk.enabled)
        {
            return process_fsk(block, frames, first_input_frame, capacity);
        }

        if (params_.markov.enabled)
        {
            return process_markov(block, frames, first_input_frame, capacity);
        }

        if (const m110::Status valid = validate_fading_process(block, frames, first_input_frame, capacity); !valid.is_ok())
        {
            return valid;
        }

        for (std::size_t index = 0U; index < frames; ++index)
        {
            const std::uint64_t absolute = first_input_frame + index;

            if (absolute < start_ || absolute >= end_)
            {
                continue;
            }

            const std::uint64_t t = absolute - start_;
            const ComplexFloat fading = next_fading_gain(t);
            double envelope = 1.0;

            if (mPulsed)
            {
                const PulseFrame pulse = pulse_frame(t, length_, params_.pulse_period_frames, params_.pulse_on_frames, ramp_);

                if (pulse.starts)
                {
                    record_pulse_start(stats_, length_ == unbounded_frames);
                }

                envelope = pulse.envelope;

                if (pulse.active)
                {
                    const double angle = radians_per_frame_ * static_cast<double>(t) + phase_radians_;
                    const float value = tone_sample(angle, envelope, fading);
                    block[index] += value;
                    record_sample(stats_, value);
                }

                continue;
            }
            else
            {
                envelope = legacy_envelope(t, length_, ramp_);
            }

            const double angle = radians_per_frame_ * static_cast<double>(t) + phase_radians_;
            const auto value = tone_sample(angle, envelope, fading);
            block[index] += value;
            record_sample(stats_, value);

            if (!mPulsed && t == 0U)
            {
                stats_.events_applied = 1U;
            }
        }

        if (mFadingEnabled && frames != 0U)
        {
            mSweepStarted = true;
            mSweepNextFrame = first_input_frame + frames;
        }

        return frames;
    }

    [[nodiscard]] const StageStats& stats() const noexcept override { return stats_; }

private:
    [[nodiscard]] bool requires_gated_validation() const noexcept { return params_.sweep.enabled || params_.markov.enabled || params_.fsk.enabled || params_.fading.enabled; }

    [[nodiscard]] bool sequential_mode() const noexcept { return params_.sweep.enabled || mMarkovEnabled || mFskEnabled || mFadingEnabled; }

    [[nodiscard]] m110::Result<std::size_t> process_sweep(float* block, std::size_t frames, std::uint64_t first_input_frame, std::size_t capacity) noexcept
    {
        if (!mSequentialPrepared)
        {
            return sweep_unprepared;
        }

        if ((frames != 0U && block == nullptr) || frames > capacity || frames > std::numeric_limits<std::uint64_t>::max() - first_input_frame)
        {
            return sweep_frame_range;
        }

        if (frames != 0U && ((!mSweepStarted && first_input_frame != 0U) || (mSweepStarted && first_input_frame != mSweepNextFrame)))
        {
            return sweep_frame_order;
        }

        for (std::size_t index = 0U; index < frames; ++index)
        {
            const std::uint64_t absolute = first_input_frame + index;

            if (absolute < start_ || absolute >= end_)
            {
                continue;
            }

            const std::uint64_t t = absolute - start_;
            const ComplexFloat fading = next_fading_gain(t);
            double envelope = 1.0;
            bool active = true;

            if (mMarkovEnabled)
            {
                if (mMarkovGate.on() && mMarkovGate.dwell_elapsed() == 0U)
                {
                    record_markov_on_start();
                }

                active = mMarkovGate.on();
                envelope = markov_envelope(t);
            }
            else if (mPulsed)
            {
                const PulseFrame pulse = pulse_frame(t, length_, params_.pulse_period_frames, params_.pulse_on_frames, ramp_);

                if (pulse.starts)
                {
                    record_pulse_start(stats_, length_ == unbounded_frames);
                }

                active = pulse.active;
                envelope = pulse.envelope;
            }
            else
            {
                envelope = legacy_envelope(t, length_, ramp_);

                if (t == 0U)
                {
                    stats_.events_applied = 1U;
                }
            }

            if (active)
            {
                const double angle = det::two_pi * (static_cast<double>(mCarrierPhase) / phase_turn);
                const float value = tone_sample(angle, envelope, fading);
                block[index] += value;
                record_sample(stats_, value);
            }

            if (mMarkovEnabled)
            {
                (void)mMarkovGate.advance();
            }

            const double average_hz = interval_average_cw_frequency(params_.sweep, mSweepPhase, mSweepStep);
            mCarrierPhase += carrier_phase_step(average_hz);
            mSweepPhase += mSweepStep;
        }

        if (frames != 0U)
        {
            mSweepStarted = true;
            mSweepNextFrame = first_input_frame + frames;
        }

        return frames;
    }

    [[nodiscard]] m110::Result<std::size_t> process_markov(float* block, std::size_t frames, std::uint64_t first_input_frame, std::size_t capacity) noexcept
    {
        if (!mSequentialPrepared)
        {
            return gated_unprepared;
        }

        if ((frames != 0U && block == nullptr) || frames > capacity || frames > std::numeric_limits<std::uint64_t>::max() - first_input_frame)
        {
            return gated_frame_range;
        }

        if (frames != 0U && ((!mSweepStarted && first_input_frame != 0U) || (mSweepStarted && first_input_frame != mSweepNextFrame)))
        {
            return gated_frame_order;
        }

        for (std::size_t index = 0U; index < frames; ++index)
        {
            const std::uint64_t absolute = first_input_frame + index;

            if (absolute < start_ || absolute >= end_)
            {
                continue;
            }

            const std::uint64_t t = absolute - start_;
            const ComplexFloat fading = next_fading_gain(t);

            if (mMarkovGate.on() && mMarkovGate.dwell_elapsed() == 0U)
            {
                record_markov_on_start();
            }

            if (mMarkovGate.on())
            {
                const double envelope = markov_envelope(t);
                const double angle = radians_per_frame_ * static_cast<double>(t) + phase_radians_;
                const float value = tone_sample(angle, envelope, fading);
                block[index] += value;
                record_sample(stats_, value);
            }

            (void)mMarkovGate.advance();
        }

        if (frames != 0U)
        {
            mSweepStarted = true;
            mSweepNextFrame = first_input_frame + frames;
        }

        return frames;
    }

    [[nodiscard]] m110::Status configure_fsk(std::uint64_t seed, std::uint32_t index) noexcept
    {
        const m110::Result<std::uint64_t> period = cw_fsk_symbol_frames(params_.fsk);

        if (!period.has_value())
        {
            return period.error();
        }

        mFskSymbolFrames = period.value();
        mFskSymbolRemaining = mFskSymbolFrames;
        const double half_shift = params_.fsk.shift.in(units::Hz) / 2.0;
        mFskMarkStep = carrier_phase_step(params_.frequency_hz + half_shift);
        mFskSpaceStep = carrier_phase_step(params_.frequency_hz - half_shift);
        mFskMark = true;

        if (params_.fsk.pattern == FskPattern::random)
        {
            mFskGenerator.reseed(seed, cw_fsk_rng_family, index);
            mFskMark = mFskGenerator.uniform() >= 0.5;
        }

        return m110::Status::success();
    }

    [[nodiscard]] m110::Status configure_optional_generators(const StageContext& context) noexcept
    {
        const StageRandomKey key = context.random_key();

        if (mMarkovEnabled)
        {
            if (const m110::Status configured = mMarkovGate.configure(params_.markov, key.seed, key.index); !configured.is_ok())
            {
                return configured;
            }
        }

        if (mFskEnabled)
        {
            if (const m110::Status configured = configure_fsk(key.seed, key.index); !configured.is_ok())
            {
                return configured;
            }
        }

        if (params_.fading.enabled)
        {
            if (const m110::Status configured = configure_cw_fading(mFadingPath, params_.fading, key.seed, key.index); !configured.is_ok())
            {
                return configured;
            }

            mFadingEnabled = true;
        }

        return m110::Status::success();
    }

    [[nodiscard]] m110::Result<std::size_t> process_fsk(float* block, std::size_t frames, std::uint64_t first_input_frame, std::size_t capacity) noexcept
    {
        if (const m110::Status valid = validate_fsk_process(block, frames, first_input_frame, capacity); !valid.is_ok())
        {
            return valid;
        }

        for (std::size_t index = 0U; index < frames; ++index)
        {
            const std::uint64_t absolute = first_input_frame + index;

            if (absolute < start_ || absolute >= end_)
            {
                continue;
            }

            process_fsk_sample(block, index, absolute);
        }

        if (frames != 0U)
        {
            mSweepStarted = true;
            mSweepNextFrame = first_input_frame + frames;
        }

        return frames;
    }

    [[nodiscard]] m110::Status validate_fsk_process(const float* block, std::size_t frames, std::uint64_t first_input_frame, std::size_t capacity) const noexcept
    {
        if (!mSequentialPrepared)
        {
            return fsk_unprepared;
        }

        if ((frames != 0U && block == nullptr) || frames > capacity || frames > std::numeric_limits<std::uint64_t>::max() - first_input_frame)
        {
            return fsk_frame_range;
        }

        if (frames != 0U && ((!mSweepStarted && first_input_frame != 0U) || (mSweepStarted && first_input_frame != mSweepNextFrame)))
        {
            return fsk_frame_order;
        }

        return m110::Status::success();
    }

    void process_fsk_sample(float* block, std::size_t index, std::uint64_t absolute) noexcept
    {
        const std::uint64_t t = absolute - start_;
        const PulseFrame pulse = (!mMarkovEnabled && mPulsed) ? pulse_frame(t, length_, params_.pulse_period_frames, params_.pulse_on_frames, ramp_) : PulseFrame{};

        if (!mMarkovEnabled && pulse.starts)
        {
            record_pulse_start(stats_, length_ == unbounded_frames);
        }

        const bool active = mMarkovEnabled ? markov_on_at_sample() : (mPulsed ? pulse.active : true);
        const double envelope = mMarkovEnabled ? markov_envelope(t) : (mPulsed ? pulse.envelope : legacy_envelope(t, length_, ramp_));

        if (!mMarkovEnabled && !mPulsed && t == 0U)
        {
            stats_.events_applied = 1U;
        }

        const ComplexFloat fading = next_fading_gain(t);

        if (active)
        {
            const double angle = det::two_pi * (static_cast<double>(mCarrierPhase) / phase_turn);
            const float value = tone_sample(angle, envelope, fading);
            block[index] += value;
            record_sample(stats_, value);
        }

        advance_fsk_state();
    }

    [[nodiscard]] bool markov_on_at_sample() noexcept
    {
        if (mMarkovGate.on() && mMarkovGate.dwell_elapsed() == 0U)
        {
            record_markov_on_start();
        }

        return mMarkovGate.on();
    }

    void advance_fsk_state() noexcept
    {
        mCarrierPhase += mFskMark ? mFskMarkStep : mFskSpaceStep;
        --mFskSymbolRemaining;

        if (mFskSymbolRemaining == 0U)
        {
            mFskSymbolRemaining = mFskSymbolFrames;
            advance_fsk_symbol();
        }

        if (mMarkovEnabled)
        {
            (void)mMarkovGate.advance();
        }
    }

    void advance_fsk_symbol() noexcept
    {
        if (params_.fsk.pattern == FskPattern::alternating)
        {
            mFskMark = !mFskMark;
        }
        else
        {
            mFskMark = mFskGenerator.uniform() >= 0.5;
        }
    }

    [[nodiscard]] double markov_envelope(std::uint64_t elapsed) const noexcept
    {
        std::uint64_t on_length = mMarkovGate.dwell_frames();

        if (length_ != unbounded_frames)
        {
            const std::uint64_t remaining = length_ - elapsed;
            const std::uint64_t truncated_length = mMarkovGate.dwell_elapsed() + remaining;

            if (truncated_length < on_length)
            {
                on_length = truncated_length;
            }
        }

        return pulse_envelope(mMarkovGate.dwell_elapsed(), on_length, ramp_);
    }

    void record_markov_on_start() noexcept
    {
        count_event(stats_.events_scheduled);
        count_event(stats_.events_applied);
    }

    [[nodiscard]] ComplexFloat next_fading_gain(std::uint64_t elapsed) noexcept { return mFadingEnabled ? mFadingPath.next_gain(units::SampleIndex{elapsed}, nullptr) : ComplexFloat{1.0F, 0.0F}; }

    [[nodiscard]] float tone_sample(double angle, double envelope, ComplexFloat fading) const noexcept
    {
        const double in_phase = det::sin(angle);

        if (!mFadingEnabled)
        {
            return static_cast<float>(amplitude_ * in_phase * envelope);
        }

        const double quadrature = det::cos(angle);
        return static_cast<float>(amplitude_ * (static_cast<double>(fading.real) * in_phase + static_cast<double>(fading.imag) * quadrature) * envelope);
    }

    [[nodiscard]] m110::Status validate_fading_process(const float* block, std::size_t frames, std::uint64_t first_input_frame, std::size_t capacity) const noexcept
    {
        if (!params_.fading.enabled)
        {
            return m110::Status::success();
        }

        if (!mSequentialPrepared)
        {
            return gated_unprepared;
        }

        if ((frames != 0U && block == nullptr) || frames > capacity || frames > std::numeric_limits<std::uint64_t>::max() - first_input_frame)
        {
            return gated_frame_range;
        }

        if (frames != 0U && ((!mSweepStarted && first_input_frame != 0U) || (mSweepStarted && first_input_frame != mSweepNextFrame)))
        {
            return gated_frame_order;
        }

        return m110::Status::success();
    }

    CwParams params_;
    std::uint64_t start_{};
    std::uint64_t end_{};
    std::uint64_t length_{};
    std::uint64_t ramp_{};
    bool mPulsed{};
    double phase_radians_{};
    double amplitude_{};
    double radians_per_frame_{};
    std::uint64_t mSweepStep{};
    std::uint64_t mSweepPhase{};
    std::uint64_t mCarrierPhase{};
    std::uint64_t mSweepNextFrame{};
    bool mSweepStarted{};
    bool mSequentialPrepared{};
    bool mMarkovEnabled{};
    MarkovGate mMarkovGate{};
    WattersonPathState mFadingPath{};
    det::Pcg32 mFskGenerator{};
    std::uint64_t mFskSymbolFrames{};
    std::uint64_t mFskSymbolRemaining{};
    std::uint64_t mFskMarkStep{};
    std::uint64_t mFskSpaceStep{};
    bool mFskEnabled{};
    bool mFskMark{true};
    bool mFadingEnabled{};
    StageStats stats_{};
};

static_assert(alignof(CwImpairment) <= impairment_alignment);

} // namespace

std::size_t cw_size() noexcept
{
    return align_storage(sizeof(CwImpairment));
}

PlacedStage construct_cw(const CwParams& params, void* storage, std::size_t storage_bytes) noexcept
{
    if (storage == nullptr || reinterpret_cast<std::uintptr_t>(storage) % impairment_alignment != 0U || storage_bytes < cw_size())
    {
        return placement_refused;
    }

    return std::ref<Impairment>(*new (storage) CwImpairment(params));
}

} // namespace signal_lab
