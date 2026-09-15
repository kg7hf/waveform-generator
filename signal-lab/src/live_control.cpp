// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Paul R. Decker

// Live sample controller
// Guide: docs/modules/live-control-and-replay.md
// Ownership: Single owner applies bounded controls at output-frame positions.
// Contract: No I/O or allocation while processing; retain acknowledged events for reproduction.

#include "signal_lab/live_control.hpp"

#include <cmath>

namespace signal_lab
{
namespace
{

constexpr std::uint64_t static_tau_frames = 960U; // 20 ms at 48 kHz.
constexpr std::uint64_t static_length_frames = 8U * static_tau_frames;
constexpr double static_frequency_hz = 1700.0;
constexpr double static_phase_step = det::two_pi * static_frequency_hz / sample_rate_hz;

bool fail(const char** error, const char* message) noexcept
{
    if (error != nullptr)
    {
        *error = message;
    }

    return false;
}

bool in_range(double value, double minimum, double maximum) noexcept
{
    return std::isfinite(value) && value >= minimum && value <= maximum;
}

} // namespace

const char* control_result_name(ControlResult result) noexcept
{
    switch (result)
    {
        case ControlResult::accepted:
            return "accepted";

        case ControlResult::not_configured:
            return "not_configured";

        case ControlResult::invalid_event:
            return "invalid_event";

        case ControlResult::late_event:
            return "late_event";

        case ControlResult::pending_full:
            return "pending_full";

        case ControlResult::capture_full:
            return "capture_full";
    }

    return "invalid_event";
}

const char* control_kind_name(ControlKind kind) noexcept
{
    switch (kind)
    {
        case ControlKind::cw_enable:
            return "cw_enable";

        case ControlKind::cw_frequency:
            return "cw_frequency";

        case ControlKind::cw_ci:
            return "cw_ci";

        case ControlKind::static_enable:
            return "static_enable";

        case ControlKind::static_rate:
            return "static_rate";

        case ControlKind::static_peak:
            return "static_peak";

        case ControlKind::fade_now:
            return "fade_now";
    }

    return "invalid";
}

bool LiveController::reset(std::uint64_t seed, double reference_rms, const char** error) noexcept
{
    if (!std::isfinite(reference_rms) || !(reference_rms > 0.0) || reference_rms > 32768.0)
    {
        return fail(error, "live reference RMS must be finite and in (0, 32768]");
    }

    // Explicit member reset avoids a multi-kilobyte temporary on embedded stacks.
    configured_ = true;
    seed_ = seed;
    reference_rms_ = reference_rms;
    state_ = LiveState{};
    stats_ = LiveStats{};
    pending_count_ = 0U;
    capture_count_ = 0U;

    for (std::size_t index = 0U; index < live_cw_capacity; ++index)
    {
        cw_phase_[index] = 0.0;
        cw_step_[index] = det::two_pi * state_.cw[index].frequency_hz / sample_rate_hz;
        cw_amplitude_[index] = std::sqrt(2.0) * reference_rms_ * det::db_to_amplitude(-state_.cw[index].ci_db);
    }

    next_static_frame_ = unbounded_frames;
    static_decay_ = det::exp(-1.0 / static_cast<double>(static_tau_frames));
    // Live static draws are separate from every scenario stage and CW control.
    static_schedule_rng_.reseed(seed, 0x4C530001U, 0U);
    static_phase_rng_.reseed(seed, 0x4C530002U, 0U);

    for (auto& crash : crashes_)
    {
        crash = StaticCrash{};
    }

    digest_.reset();
    stats_.output_digest = digest_.value();
    return true;
}

ControlResult LiveController::validate(const ControlEvent& event) const noexcept
{
    if (!configured_)
    {
        return ControlResult::not_configured;
    }

    if (event.frame < stats_.frames)
    {
        return ControlResult::late_event;
    }

    if (event.kind != ControlKind::fade_now && event.duration_frames != 0U)
    {
        return ControlResult::invalid_event;
    }

    const bool cw_control = event.kind == ControlKind::cw_enable || event.kind == ControlKind::cw_frequency || event.kind == ControlKind::cw_ci;

    if ((cw_control && event.oscillator >= live_cw_capacity) || (!cw_control && event.oscillator != 0U))
    {
        return ControlResult::invalid_event;
    }

    bool valid = false;

    switch (event.kind)
    {
        case ControlKind::cw_enable:
        case ControlKind::static_enable:
            valid = event.value == 0.0 || event.value == 1.0;
            break;

        case ControlKind::cw_frequency:
            valid = std::isfinite(event.value) && event.value > 0.0 && event.value < sample_rate_hz / 2.0;
            break;

        case ControlKind::cw_ci:
            valid = in_range(event.value, -120.0, 120.0);
            break;

        case ControlKind::static_rate:
            valid = in_range(event.value, 0.001, 1000.0);
            break;

        case ControlKind::static_peak:
            valid = in_range(event.value, -120.0, 60.0);
            break;

        case ControlKind::fade_now:
            valid = in_range(event.value, 0.0, 120.0) && event.duration_frames != 0U && event.duration_frames <= unbounded_frames - event.frame;
            break;
    }

    return valid ? ControlResult::accepted : ControlResult::invalid_event;
}

void LiveController::insert(const ControlEvent& event) noexcept
{
    const auto capture_index = static_cast<std::uint16_t>(capture_count_);
    capture_[capture_count_++] = event;
    std::size_t position = pending_count_;

    while (position > 0U && capture_[pending_[position - 1U]].frame > event.frame)
    {
        pending_[position] = pending_[position - 1U];
        --position;
    }

    pending_[position] = capture_index;
    ++pending_count_;
    ++stats_.controls_accepted;
}

ControlResult LiveController::enqueue(const ControlEvent& event) noexcept
{
    const ControlResult result = validate(event);

    if (result != ControlResult::accepted)
    {
        return result;
    }

    if (pending_count_ == live_pending_capacity)
    {
        return ControlResult::pending_full;
    }

    if (capture_count_ == live_capture_capacity)
    {
        return ControlResult::capture_full;
    }

    insert(event);
    return ControlResult::accepted;
}

ControlResult LiveController::enqueue_sweep(const ControlSweep& sweep) noexcept
{
    if (!configured_)
    {
        return ControlResult::not_configured;
    }

    if (sweep.count == 0U || sweep.count > live_sweep_capacity || sweep.step_frames == 0U ||
            (sweep.kind != ControlKind::cw_frequency && sweep.kind != ControlKind::cw_ci && sweep.kind != ControlKind::fade_now) ||
            (sweep.count - 1U) > (unbounded_frames - sweep.first_frame) / sweep.step_frames)
    {
        return ControlResult::invalid_event;
    }

    for (std::size_t index = 0U; index < sweep.count; ++index)
    {
        const ControlEvent event{sweep.first_frame + index * sweep.step_frames, sweep.kind, sweep.values[index],
                  sweep.fade_duration_frames, sweep.oscillator};
        const ControlResult result = validate(event);

        if (result != ControlResult::accepted)
        {
            return result;
        }
    }

    if (sweep.count > live_pending_capacity - pending_count_)
    {
        return ControlResult::pending_full;
    }

    if (sweep.count > live_capture_capacity - capture_count_)
    {
        return ControlResult::capture_full;
    }

    for (std::size_t index = 0U; index < sweep.count; ++index)
    {
        insert({sweep.first_frame + index * sweep.step_frames, sweep.kind, sweep.values[index],
                sweep.fade_duration_frames, sweep.oscillator
               });
    }

    return ControlResult::accepted;
}

void LiveController::schedule_static(std::uint64_t after_frame) noexcept
{
    const double rounded = det::round_half_even(static_schedule_rng_.exponential(sample_rate_hz / state_.static_rate_per_second));
    const auto delay = rounded < 1.0 ? 1U : static_cast<std::uint64_t>(rounded);
    next_static_frame_ = delay > unbounded_frames - after_frame ? unbounded_frames : after_frame + delay;
}

void LiveController::apply(const ControlEvent& event) noexcept
{
    switch (event.kind)
    {
        case ControlKind::cw_enable:
            state_.cw[event.oscillator].enabled = event.value == 1.0;
            break;

        case ControlKind::cw_frequency:
            state_.cw[event.oscillator].frequency_hz = event.value;
            cw_step_[event.oscillator] = det::two_pi * event.value / sample_rate_hz;
            break;

        case ControlKind::cw_ci:
            state_.cw[event.oscillator].ci_db = event.value;
            cw_amplitude_[event.oscillator] = std::sqrt(2.0) * reference_rms_ * det::db_to_amplitude(-event.value);
            break;

        case ControlKind::static_enable:
            if (event.value == 1.0 && !state_.static_enabled)
            {
                schedule_static(event.frame);
            }

            state_.static_enabled = event.value == 1.0;

            if (!state_.static_enabled)
            {
                next_static_frame_ = unbounded_frames;

                for (auto& crash : crashes_)
                {
                    crash.active = false;
                }
            }

            break;

        case ControlKind::static_rate:
            state_.static_rate_per_second = event.value;

            if (state_.static_enabled)
            {
                schedule_static(event.frame);
            }

            break;

        case ControlKind::static_peak:
            state_.static_peak_db = event.value;
            break;

        case ControlKind::fade_now:
            // One live fade: an explicit replacement starts a new envelope at this frame.
            state_.fade_active = true;
            state_.fade_depth_db = event.value;
            state_.fade_start_frame = event.frame;
            state_.fade_duration_frames = event.duration_frames;
            break;
    }

    ++stats_.controls_applied;
}

double LiveController::fade_gain(std::uint64_t frame) noexcept
{
    if (!state_.fade_active)
    {
        return 1.0;
    }

    const std::uint64_t elapsed = frame - state_.fade_start_frame;

    if (elapsed >= state_.fade_duration_frames)
    {
        state_.fade_active = false;
        return 1.0;
    }

    const std::uint64_t attack = state_.fade_duration_frames / 4U;
    const std::uint64_t hold_end = state_.fade_duration_frames - attack;
    double depth = 1.0;

    if (elapsed < attack)
    {
        depth = 0.5 * (1.0 - det::cos(det::pi * static_cast<double>(elapsed) / static_cast<double>(attack)));
    }
    else if (elapsed >= hold_end)
    {
        depth = 0.5 * (1.0 + det::cos(det::pi * static_cast<double>(elapsed - hold_end) / static_cast<double>(attack)));
    }

    return det::db_to_amplitude(-state_.fade_depth_db * depth);
}

double LiveController::static_sample(std::uint64_t frame) noexcept
{
    if (!state_.static_enabled)
    {
        return 0.0;
    }

    StaticCrash* free_slot = nullptr;

    for (auto& crash : crashes_)
    {
        if (crash.active && crash.end_frame <= frame)
        {
            crash.active = false;
        }

        if (!crash.active && free_slot == nullptr)
        {
            free_slot = &crash;
        }
    }

    if (frame == next_static_frame_)
    {
        const double phase = static_phase_rng_.uniform() * det::two_pi;

        if (free_slot != nullptr)
        {
            free_slot->active = true;
            free_slot->end_frame = static_length_frames > unbounded_frames - frame ? unbounded_frames : frame + static_length_frames;
            free_slot->amplitude = reference_rms_ * det::db_to_amplitude(state_.static_peak_db);
            free_slot->envelope = 1.0;
            free_slot->phase = phase;
            ++stats_.static_events_started;
        }
        else
        {
            ++stats_.static_events_dropped;
        }

        schedule_static(frame);
    }

    double value = 0.0;

    for (auto& crash : crashes_)
    {
        if (crash.active)
        {
            value += crash.amplitude * crash.envelope * det::cos(crash.phase);
            crash.envelope *= static_decay_;
            crash.phase += static_phase_step;

            if (crash.phase >= det::two_pi)
            {
                crash.phase -= det::two_pi;
            }
        }
    }

    return value;
}

bool LiveController::process(std::int16_t* pcm, std::size_t frames, const char** error) noexcept
{
    if (!configured_)
    {
        return fail(error, "live controller is not configured");
    }

    if ((pcm == nullptr && frames != 0U) || frames > engine_capacity_frames || frames > unbounded_frames - stats_.frames)
    {
        return fail(error, "invalid live PCM block or output timeline overflow");
    }

    for (std::size_t index = 0U; index < frames; ++index)
    {
        const std::uint64_t absolute = stats_.frames + index;

        while (pending_count_ != 0U && capture_[pending_[0]].frame == absolute)
        {
            apply(capture_[pending_[0]]);
            --pending_count_;

            for (std::size_t pending = 0U; pending < pending_count_; ++pending)
            {
                pending_[pending] = pending_[pending + 1U];
            }
        }

        double sample = static_cast<double>(pcm[index]) / 32768.0 * fade_gain(absolute);

        for (std::size_t oscillator = 0U; oscillator < live_cw_capacity; ++oscillator)
        {
            if (state_.cw[oscillator].enabled)
            {
                sample += cw_amplitude_[oscillator] * det::sin(cw_phase_[oscillator]);
            }

            cw_phase_[oscillator] += cw_step_[oscillator];

            if (cw_phase_[oscillator] >= det::two_pi)
            {
                cw_phase_[oscillator] -= det::two_pi;
            }
        }

        sample += static_sample(absolute);
        const double scaled = sample * 32768.0;

        if (scaled < -32768.0 || scaled > 32767.0)
        {
            ++stats_.clipped_samples;
        }

        double rounded = det::round_half_even(scaled);

        if (rounded < -32768.0)
        {
            rounded = -32768.0;
        }
        else if (rounded > 32767.0)
        {
            rounded = 32767.0;
        }

        pcm[index] = static_cast<std::int16_t>(rounded);
        const double emitted = static_cast<double>(pcm[index]) / 32768.0;
        const auto magnitude = static_cast<float>(emitted < 0.0 ? -emitted : emitted);

        if (magnitude > stats_.peak)
        {
            stats_.peak = magnitude;
        }

        stats_.output_energy += emitted * emitted;
    }

    digest_.update(pcm, frames);
    stats_.output_digest = digest_.value();
    stats_.frames += frames;

    if (state_.fade_active && stats_.frames - state_.fade_start_frame >= state_.fade_duration_frames)
    {
        state_.fade_active = false;
    }

    return true;
}

bool LiveController::process(float* pcm, std::size_t frames, const char** error) noexcept
{
    if (!configured_)
    {
        return fail(error, "live controller is not configured");
    }

    if ((pcm == nullptr && frames != 0U) || frames > engine_capacity_frames ||
            frames > unbounded_frames - stats_.frames)
    {
        return fail(error, "invalid live float block or output timeline overflow");
    }

    for (std::size_t index = 0U; index < frames; ++index)
    {
        if (!std::isfinite(pcm[index]))
        {
            return fail(error, "nonfinite live sample");
        }
    }

    constexpr double minimum = -1.0;
    constexpr double maximum = static_cast<double>(waveform_generator::audio::pcm24_max) /
                               waveform_generator::audio::pcm24_scale;

    for (std::size_t index = 0U; index < frames; ++index)
    {
        const std::uint64_t absolute = stats_.frames + index;

        while (pending_count_ != 0U && capture_[pending_[0]].frame == absolute)
        {
            apply(capture_[pending_[0]]);
            --pending_count_;

            for (std::size_t pending = 0U; pending < pending_count_; ++pending)
            {
                pending_[pending] = pending_[pending + 1U];
            }
        }

        double sample = static_cast<double>(pcm[index]) * fade_gain(absolute);

        for (std::size_t oscillator = 0U; oscillator < live_cw_capacity; ++oscillator)
        {
            if (state_.cw[oscillator].enabled)
            {
                sample += cw_amplitude_[oscillator] * det::sin(cw_phase_[oscillator]);
            }

            cw_phase_[oscillator] += cw_step_[oscillator];

            if (cw_phase_[oscillator] >= det::two_pi)
            {
                cw_phase_[oscillator] -= det::two_pi;
            }
        }

        sample += static_sample(absolute);

        if (sample < minimum || sample > maximum)
        {
            ++stats_.clipped_samples;
        }

        if (sample < minimum)
        {
            sample = minimum;
        }
        else if (sample > maximum)
        {
            sample = maximum;
        }

        pcm[index] = static_cast<float>(sample);
        const double emitted = static_cast<double>(det::quantize_pcm24(pcm[index])) /
                               waveform_generator::audio::pcm24_scale;
        const auto magnitude = static_cast<float>(emitted < 0.0 ? -emitted : emitted);

        if (magnitude > stats_.peak)
        {
            stats_.peak = magnitude;
        }

        stats_.output_energy += emitted * emitted;
    }

    digest_.update(pcm, frames);
    stats_.output_digest = digest_.value();
    stats_.frames += frames;

    if (state_.fade_active &&
            stats_.frames - state_.fade_start_frame >= state_.fade_duration_frames)
    {
        state_.fade_active = false;
    }

    return true;
}

} // namespace signal_lab
