// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Paul R. Decker

#pragma once
#include "common/live_protocol.hpp"
#include "signal_lab/live_control.hpp"

namespace waveform_generator::live_protocol
{
static_assert(cw_oscillator_capacity == signal_lab::live_cw_capacity);
inline bool is_control(Kind kind) noexcept
{
    return kind >= Kind::cw_on;
}
// Shared translation for firmware and offline replay. The caller owns controller.
inline signal_lab::ControlResult apply_control(const Request& r, signal_lab::LiveController& live,
        std::uint64_t& frame, std::uint32_t& count) noexcept
{
    frame = r.scheduled ? r.frame : live.frame();
    count = 1;
    signal_lab::ControlKind kind{};

    switch (r.kind)
    {
        case Kind::cw_on:
            kind = signal_lab::ControlKind::cw_enable;
            break;

        case Kind::cw_frequency:
        case Kind::sweep_frequency:
            kind = signal_lab::ControlKind::cw_frequency;
            break;

        case Kind::cw_ci:
        case Kind::sweep_ci:
            kind = signal_lab::ControlKind::cw_ci;
            break;

        case Kind::static_on:
            kind = signal_lab::ControlKind::static_enable;
            break;

        case Kind::static_rate:
            kind = signal_lab::ControlKind::static_rate;
            break;

        case Kind::static_peak:
            kind = signal_lab::ControlKind::static_peak;
            break;

        case Kind::fade:
        case Kind::sweep_fade:
            kind = signal_lab::ControlKind::fade_now;
            break;

        default:
            return signal_lab::ControlResult::invalid_event;
    }

    if (r.kind >= Kind::sweep_frequency)
    {
        signal_lab::ControlSweep sweep{};
        sweep.first_frame = frame;
        sweep.kind = kind;
        sweep.count = r.steps;
        sweep.step_frames = static_cast<std::uint64_t>(r.interval_ms) * 48;
        sweep.fade_duration_frames = static_cast<std::uint64_t>(r.duration_ms) * 48;
        sweep.oscillator = r.oscillator;

        if (r.steps < 2 || r.steps > signal_lab::live_sweep_capacity)
        {
            return signal_lab::ControlResult::invalid_event;
        }

        for (std::uint32_t i = 0; i < r.steps; ++i)
        {
            sweep.values[i] = i == r.steps - 1 ? r.end_value : r.value + (r.end_value - r.value) * static_cast<double>(i) / static_cast<double>(r.steps - 1);
        }

        count = r.steps;
        return live.enqueue_sweep(sweep);
    }

    return live.enqueue({frame, kind, r.value, static_cast<std::uint64_t>(r.duration_ms) * 48, r.oscillator});
}
}
