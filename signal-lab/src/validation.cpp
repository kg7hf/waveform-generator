// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// Scenario safety boundary
// Guide: docs/modules/signal-lab-engine.md
// Ownership: Accepts parsed or programmatic descriptors; allocates no storage.
// Contract: Check counts, finite values and schedule bounds before indexing or integer conversion.

#include "cw_pulse_scenario.hpp"
#include "frequency_shift_scenario.hpp"
#include "second_source_scenario.hpp"
#include "signal_lab/awgn.hpp"
#include "signal_lab/cw_fading.hpp"
#include "signal_lab/cw_fsk.hpp"
#include "signal_lab/cw_sweep.hpp"
#include "signal_lab/markov_gate.hpp"
#include "signal_lab/scenario.hpp"
#include "watterson_scenario.hpp"

#include <cmath>
#include <span>

namespace signal_lab
{
namespace
{
// Keep sums/products in schedule arithmetic far below uint64_t overflow.
// This is a numerical guard, not a validated playback-duration claim.
constexpr double maximum_seconds = 1.0e9;

bool in_range(double value, double low, double high) noexcept
{
    return std::isfinite(value) && value >= low && value <= high;
}

// A scenario the engine cannot run: invalid_configuration with the static message.
[[nodiscard]] m110::Status invalid(const char* text) noexcept
{
    return m110::Status{.code = m110::StatusCode::invalid_configuration, .message = text};
}

bool valid_window(const Window& window) noexcept
{
    return in_range(window.start_seconds, 0.0, maximum_seconds) && (!window.has_duration || in_range(window.duration_seconds, 0.0, maximum_seconds));
}

[[nodiscard]] m110::Status validate_cw_stage(const CwParams& params) noexcept
{
    if (!in_range(params.ci_db, -120.0, 120.0) || !in_range(params.phase_degrees, -360.0, 360.0) || !in_range(params.ramp_seconds, 0.0, maximum_seconds) || !valid_window(params.window))
    {
        return invalid("invalid CW frequency, level, phase, or window");
    }

    if (params.fsk.enabled && params.sweep.enabled)
    {
        return invalid("CW FSK cannot combine with a frequency sweep");
    }

    if (params.sweep.enabled)
    {
        if (params.frequency_hz != 0.0)
        {
            return invalid("CW sweep cannot also set a fixed frequency");
        }

        if (const m110::Status sweep = validate_cw_sweep_params(params.sweep); !sweep.is_ok())
        {
            return sweep;
        }
    }
    else if (!in_range(params.frequency_hz, 1.0, sample_rate_hz / 2.0 - 1.0))
    {
        return invalid("invalid CW frequency, level, phase, or window");
    }

    if (const m110::Status pulse = validate_cw_pulse_timing(params); !pulse.is_ok())
    {
        return pulse;
    }

    if (params.markov.enabled && (params.pulse_period_frames != 0U || params.pulse_on_frames != 0U))
    {
        return invalid("CW Markov gate cannot combine with a periodic pulse schedule");
    }

    if (const m110::Status markov = validate_markov_gate_params(params.markov); !markov.is_ok())
    {
        return markov;
    }

    if (const m110::Status fsk = validate_cw_fsk_params(params.fsk, params.frequency_hz); !fsk.is_ok())
    {
        return fsk;
    }

    return validate_cw_fading_params(params.fading);
}

// The stages the engine builds in typed slots rather than in the placement arena
// (awgn.hpp, watterson.hpp). Their rules live here and in their own files, outside
// the legacy switch.
[[nodiscard]] m110::Status valid_typed_stage(const Scenario& scenario, std::uint32_t index, std::uint32_t& awgn_stages) noexcept
{
    const Stage& stage = std::span{scenario.stages}[index];

    switch (stage.type)
    {
    case StageType::awgn:
        ++awgn_stages;

        if (awgn_stages > max_awgn_stages)
        {
            return invalid("awgn: at most 2 stages per scenario");
        }

        return in_range(stage.awgn.snr.in(units::dB), -awgn_max_snr.in(units::dB), awgn_max_snr.in(units::dB)) && valid_window(stage.awgn.window) ? m110::Status::success()
                                                                                                                                                  : invalid("invalid AWGN level or window");

    case StageType::watterson:
        return validate_watterson_stage(scenario, index);

    case StageType::frequency_shift:
        return validate_frequency_shift_stage(scenario, index);

    case StageType::second_source:
        return validate_second_source_stage(scenario, index);

    default:
        return invalid("unknown impairment type");
    }
}

[[nodiscard]] m110::Status valid_stream_ids(const Scenario& scenario) noexcept
{
    for (std::uint32_t index = 0U; index < scenario.stage_count; ++index)
    {
        const std::uint32_t stream_id = scenario.stages[index].stream_id;

        if (stream_id == 0U)
        {
            continue;
        }

        for (std::uint32_t previous = 0U; previous < index; ++previous)
        {
            if (scenario.stages[previous].stream_id == stream_id)
            {
                return invalid("impairment stream_id values must be unique");
            }
        }
    }

    return m110::Status::success();
}

[[nodiscard]] m110::Status valid_geometry(const Scenario& scenario) noexcept
{
    if (!valid_noise_bandwidth(scenario.noise_bandwidth))
    {
        return invalid("noise_bandwidth_hz must be 3000, 6000, 9000 or 12000");
    }

    if (scenario.stage_count > max_stages || !in_range(scenario.source_gain_db, -120.0, 120.0) || !in_range(scenario.reference_start_seconds, 0.0, maximum_seconds) ||
        !in_range(scenario.reference_duration_seconds, 0.0, maximum_seconds) || (scenario.has_reference_rms && (!std::isfinite(scenario.reference_rms) || scenario.reference_rms <= 0.0)))
    {
        return invalid("invalid scenario geometry, gain, or reference");
    }

    return valid_stream_ids(scenario);
}
} // namespace

m110::Status validate_scenario(const Scenario& scenario) noexcept
{
    if (const m110::Status geometry = valid_geometry(scenario); !geometry.is_ok())
    {
        return geometry;
    }

    std::uint32_t awgn_stages = 0U;

    for (std::uint32_t index = 0U; index < scenario.stage_count; ++index)
    {
        const auto& stage = scenario.stages[index];

        switch (stage.type)
        {
        case StageType::cw:
        {
            if (const m110::Status cw = validate_cw_stage(stage.cw); !cw.is_ok())
            {
                return cw;
            }

            break;
        }

        case StageType::impulse:
        {
            const auto& p = stage.impulse;

            if (!in_range(p.peak_db, -120.0, 120.0) || !in_range(p.decay_ms, 0.000001, maximum_seconds) || !in_range(p.truncate_tau, 0.000001, 1000.0) ||
                !in_range(p.ring_hz, 0.0, sample_rate_hz / 2.0 - 1.0) || !in_range(p.phase_degrees, -360.0, 360.0) || !valid_window(p.window) || !in_range(p.first_seconds, 0.0, maximum_seconds) ||
                (p.poisson ? !in_range(p.rate_per_sec, 0.000001, sample_rate_hz) : !in_range(p.period_seconds, 1.0 / sample_rate_hz, maximum_seconds)))
            {
                return invalid("invalid impulse shape or schedule");
            }

            break;
        }

        case StageType::fade:
        {
            const auto& p = stage.fade;

            if (p.start_count > max_events || !in_range(p.depth_db, 0.0, 120.0) || !valid_window(p.window) || !in_range(p.attack_ms, 0.0, maximum_seconds) ||
                !in_range(p.hold_ms, 0.0, maximum_seconds) || !in_range(p.recovery_ms, 0.0, maximum_seconds) || !in_range(p.duration_ms, 0.0, maximum_seconds) ||
                !in_range(p.first_seconds, 0.0, maximum_seconds))
            {
                return invalid("invalid fade shape or event count");
            }

            switch (p.schedule)
            {
            case FadeSchedule::explicit_starts:
                for (std::uint32_t event = 0U; event < p.start_count; ++event)
                    if (!in_range(p.starts_seconds[event], 0.0, maximum_seconds))
                    {
                        return invalid("invalid fade start time");
                    }

                break;

            case FadeSchedule::periodic:
                if (!in_range(p.period_seconds, 1.0 / sample_rate_hz, maximum_seconds))
                {
                    return invalid("invalid fade period");
                }

                break;

            case FadeSchedule::poisson:
                if (!in_range(p.rate_per_sec, 0.000001, sample_rate_hz))
                {
                    return invalid("invalid fade rate");
                }

                break;

            default:
                return invalid("unknown fade schedule");
            }

            break;
        }

        case StageType::sample_slip:
            if (index + 1U != scenario.stage_count || stage.slip.event_count > max_events)
            {
                return invalid("sample slips must be last with at most 32 events");
            }

            for (std::uint32_t event = 0U; event < stage.slip.event_count; ++event)
            {
                const auto& p = stage.slip.events[event];

                if (!in_range(p.at_seconds, 0.0, maximum_seconds) || p.length_samples == 0U || p.length_samples > max_slip_length)
                {
                    return invalid("invalid sample slip time or length");
                }
            }

            break;

        default:
            if (const m110::Status typed = valid_typed_stage(scenario, index, awgn_stages); !typed.is_ok())
            {
                return typed;
            }

            break;
        }
    }

    return m110::Status::success();
}
} // namespace signal_lab
