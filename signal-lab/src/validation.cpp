// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Paul R. Decker

// Scenario safety boundary
// Guide: docs/modules/signal-lab-engine.md
// Ownership: Accepts parsed or programmatic descriptors; allocates no storage.
// Contract: Check counts, finite values and schedule bounds before indexing or integer conversion.

#include "signal_lab/scenario.hpp"

#include <cmath>

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

bool valid_window(const Window& window) noexcept
{
    return in_range(window.start_seconds, 0.0, maximum_seconds) &&
           (!window.has_duration || in_range(window.duration_seconds, 0.0, maximum_seconds));
}

bool invalid(const char** error, const char* text) noexcept
{
    if (error != nullptr)
    {
        *error = text;
    }

    return false;
}
}

bool validate_scenario(const Scenario& scenario, const char** error) noexcept
{
    if (scenario.stage_count > max_stages || !in_range(scenario.source_gain_db, -120.0, 120.0) ||
            !in_range(scenario.reference_start_seconds, 0.0, maximum_seconds) ||
            !in_range(scenario.reference_duration_seconds, 0.0, maximum_seconds) ||
            (scenario.has_reference_rms && (!std::isfinite(scenario.reference_rms) || scenario.reference_rms <= 0.0)))
    {
        return invalid(error, "invalid scenario geometry, gain, or reference");
    }

    for (std::uint32_t index = 0U; index < scenario.stage_count; ++index)
    {
        const auto& stage = scenario.stages[index];

        switch (stage.type)
        {
            case StageType::awgn:
                if (!in_range(stage.awgn.snr_db, -120.0, 120.0) || !valid_window(stage.awgn.window))
                {
                    return invalid(error, "invalid AWGN level or window");
                }

                break;

            case StageType::cw:
                {
                    const auto& p = stage.cw;

                    if (!in_range(p.frequency_hz, 1.0, sample_rate_hz / 2.0 - 1.0) ||
                            !in_range(p.ci_db, -120.0, 120.0) || !in_range(p.phase_degrees, -360.0, 360.0) ||
                            !in_range(p.ramp_seconds, 0.0, maximum_seconds) || !valid_window(p.window))
                    {
                        return invalid(error, "invalid CW frequency, level, phase, or window");
                    }

                    break;
                }

            case StageType::impulse:
                {
                    const auto& p = stage.impulse;

                    if (!in_range(p.peak_db, -120.0, 120.0) || !in_range(p.decay_ms, 0.000001, maximum_seconds) ||
                            !in_range(p.truncate_tau, 0.000001, 1000.0) || !in_range(p.ring_hz, 0.0, sample_rate_hz / 2.0 - 1.0) ||
                            !in_range(p.phase_degrees, -360.0, 360.0) || !valid_window(p.window) ||
                            !in_range(p.first_seconds, 0.0, maximum_seconds) ||
                            (p.poisson ? !in_range(p.rate_per_sec, 0.000001, sample_rate_hz) : !in_range(p.period_seconds, 1.0 / sample_rate_hz, maximum_seconds)))
                    {
                        return invalid(error, "invalid impulse shape or schedule");
                    }

                    break;
                }

            case StageType::fade:
                {
                    const auto& p = stage.fade;

                    if (p.start_count > max_events || !in_range(p.depth_db, 0.0, 120.0) || !valid_window(p.window) ||
                            !in_range(p.attack_ms, 0.0, maximum_seconds) || !in_range(p.hold_ms, 0.0, maximum_seconds) ||
                            !in_range(p.recovery_ms, 0.0, maximum_seconds) || !in_range(p.duration_ms, 0.0, maximum_seconds) ||
                            !in_range(p.first_seconds, 0.0, maximum_seconds))
                    {
                        return invalid(error, "invalid fade shape or event count");
                    }

                    switch (p.schedule)
                    {
                        case FadeSchedule::explicit_starts:
                            for (std::uint32_t event = 0U; event < p.start_count; ++event)
                                if (!in_range(p.starts_seconds[event], 0.0, maximum_seconds))
                                {
                                    return invalid(error, "invalid fade start time");
                                }

                            break;

                        case FadeSchedule::periodic:
                            if (!in_range(p.period_seconds, 1.0 / sample_rate_hz, maximum_seconds))
                            {
                                return invalid(error, "invalid fade period");
                            }

                            break;

                        case FadeSchedule::poisson:
                            if (!in_range(p.rate_per_sec, 0.000001, sample_rate_hz))
                            {
                                return invalid(error, "invalid fade rate");
                            }

                            break;

                        default:
                            return invalid(error, "unknown fade schedule");
                    }

                    break;
                }

            case StageType::sample_slip:
                if (index + 1U != scenario.stage_count || stage.slip.event_count > max_events)
                {
                    return invalid(error, "sample slips must be last with at most 32 events");
                }

                for (std::uint32_t event = 0U; event < stage.slip.event_count; ++event)
                {
                    const auto& p = stage.slip.events[event];

                    if (!in_range(p.at_seconds, 0.0, maximum_seconds) || p.length_samples == 0U || p.length_samples > max_slip_length)
                    {
                        return invalid(error, "invalid sample slip time or length");
                    }
                }

                break;

            default:
                return invalid(error, "unknown impairment type");
        }
    }

    return true;
}
}
