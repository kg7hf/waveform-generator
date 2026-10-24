// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// Strict CW pulse_period_seconds/pulse_duty_cycle conversion to exact frames.

#include "cw_pulse_scenario.hpp"

#include <cmath>
#include <string_view>

namespace signal_lab
{
namespace
{

constexpr double frame_alignment_tolerance_frames = 1.0e-6;

[[nodiscard]] m110::Status malformed(const char* message) noexcept
{
    return {m110::StatusCode::invalid_argument, message};
}

[[nodiscard]] m110::Status invalid(const char* message) noexcept
{
    return {m110::StatusCode::invalid_configuration, message};
}

[[nodiscard]] m110::Result<std::uint64_t> seconds_to_frames(const json::Value* value) noexcept
{
    if (value == nullptr || value->type != json::Type::number || !std::isfinite(value->number) || value->number <= 0.0)
    {
        return malformed("cw.pulse_period_seconds must be finite and positive");
    }

    const double exact_frames = value->number * static_cast<double>(sample_rate_hz);

    if (!std::isfinite(exact_frames) || exact_frames < 1.0 || exact_frames > static_cast<double>(max_cw_pulse_frames))
    {
        return malformed("cw.pulse_period_seconds is outside the supported frame range");
    }

    const double rounded_frames = std::round(exact_frames);

    if (std::abs(exact_frames - rounded_frames) > frame_alignment_tolerance_frames)
    {
        return malformed("cw.pulse_period_seconds must resolve to a whole 48 kHz frame");
    }

    return static_cast<std::uint64_t>(rounded_frames);
}

[[nodiscard]] m110::Result<std::uint64_t> duty_to_frames(const json::Value* value, std::uint64_t period_frames) noexcept
{
    if (value == nullptr || value->type != json::Type::number || !std::isfinite(value->number) || value->number <= 0.0 || value->number > 1.0)
    {
        return malformed("cw.pulse_duty_cycle must be a finite fraction in (0, 1]");
    }

    const double exact_frames = static_cast<double>(period_frames) * value->number;

    if (!std::isfinite(exact_frames) || exact_frames < 1.0 || exact_frames > static_cast<double>(period_frames))
    {
        return malformed("cw.pulse_duty_cycle must resolve to positive on-time within the period");
    }

    const double rounded_frames = std::round(exact_frames);

    if (std::abs(exact_frames - rounded_frames) > frame_alignment_tolerance_frames)
    {
        return malformed("cw.pulse_duty_cycle must resolve to a whole 48 kHz frame");
    }

    return static_cast<std::uint64_t>(rounded_frames);
}

} // namespace

m110::Status parse_cw_pulse_schedule(const json::Document& document, const json::Value& object, CwParams& params) noexcept
{
    bool has_period = false;
    bool has_duty = false;
    std::uint32_t child = object.first_child;

    for (std::uint32_t index = 0U; index < object.child_count; ++index)
    {
        const json::Value* member = document.child(child);

        if (member == nullptr)
        {
            return malformed("cw: invalid member");
        }

        const std::string_view key{member->key, member->key_length};
        bool* const found = key == "pulse_period_seconds" ? &has_period : key == "pulse_duty_cycle" ? &has_duty : nullptr;

        if (found != nullptr)
        {
            if (*found)
            {
                return malformed("cw: duplicate pulse field");
            }

            *found = true;
        }

        child = member->next_sibling;
    }

    if (!has_period && !has_duty)
    {
        params.pulse_period_frames = 0U;
        params.pulse_on_frames = 0U;
        return m110::Status::success();
    }

    if (has_period != has_duty)
    {
        return malformed("cw pulse needs both pulse_period_seconds and pulse_duty_cycle");
    }

    const auto period = seconds_to_frames(document.find(object, "pulse_period_seconds"));

    if (!period.has_value())
    {
        return period.status();
    }

    const auto on_frames = duty_to_frames(document.find(object, "pulse_duty_cycle"), period.value());

    if (!on_frames.has_value())
    {
        return on_frames.status();
    }

    params.pulse_period_frames = period.value();
    params.pulse_on_frames = on_frames.value();
    return m110::Status::success();
}

m110::Status validate_cw_pulse_timing(const CwParams& params) noexcept
{
    const bool no_pulse = params.pulse_period_frames == 0U && params.pulse_on_frames == 0U;

    if (no_pulse || (params.pulse_period_frames > 0U && params.pulse_period_frames <= max_cw_pulse_frames && params.pulse_on_frames > 0U && params.pulse_on_frames <= params.pulse_period_frames))
    {
        return m110::Status::success();
    }

    return invalid("invalid CW pulse period or on-time");
}

} // namespace signal_lab
