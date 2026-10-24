// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// Strict CW Markov ON/OFF dwell parsing. Means are expected dwell durations,
// not event times, so they need not resolve to integral sample frames.

#include "cw_markov_scenario.hpp"

#include "signal_lab/markov_gate.hpp"

#include <array>
#include <cmath>
#include <string_view>

namespace signal_lab
{
namespace
{

[[nodiscard]] m110::Status malformed(const char* message) noexcept
{
    return {m110::StatusCode::invalid_argument, message};
}

[[nodiscard]] m110::Result<double> numeric_member(const json::Document& document, const json::Value& object, const char* key) noexcept
{
    const json::Value* member = document.find(object, key);

    if (member == nullptr || member->type != json::Type::number || !std::isfinite(member->number))
    {
        return malformed("cw.markov_on_off means must be finite numbers");
    }

    return member->number;
}

[[nodiscard]] m110::Status parse_gate(const json::Document& document, const json::Value& object, CwParams& params) noexcept
{
    constexpr std::array<std::string_view, 2U> names{"mean_on_seconds", "mean_off_seconds"};
    std::array<bool, names.size()> found{};
    std::uint32_t child = object.first_child;

    for (std::uint32_t index = 0U; index < object.child_count; ++index)
    {
        const json::Value* member = document.child(child);

        if (member == nullptr)
        {
            return malformed("cw.markov_on_off: invalid member");
        }

        const std::string_view key{member->key, member->key_length};
        std::size_t name_index = 0U;

        while (name_index < names.size() && names[name_index] != key)
        {
            ++name_index;
        }

        if (name_index == names.size())
        {
            return malformed("cw.markov_on_off: unknown member");
        }

        if (found[name_index])
        {
            return malformed("cw.markov_on_off: duplicate member");
        }

        found[name_index] = true;
        child = member->next_sibling;
    }

    if (!found[0U] || !found[1U])
    {
        return malformed("cw.markov_on_off requires mean_on_seconds and mean_off_seconds");
    }

    const auto mean_on = numeric_member(document, object, "mean_on_seconds");
    const auto mean_off = numeric_member(document, object, "mean_off_seconds");

    if (!mean_on.has_value())
    {
        return mean_on.status();
    }

    if (!mean_off.has_value())
    {
        return mean_off.status();
    }

    params.markov = MarkovGateParams{
        .enabled = true,
        .mean_on_seconds = mean_on.value(),
        .mean_off_seconds = mean_off.value(),
    };
    return validate_markov_gate_params(params.markov);
}

} // namespace

m110::Status parse_cw_markov_gate(const json::Document& document, const json::Value& object, CwParams& params) noexcept
{
    bool has_gate = false;
    bool has_pulse = false;
    std::uint32_t child = object.first_child;

    for (std::uint32_t index = 0U; index < object.child_count; ++index)
    {
        const json::Value* member = document.child(child);

        if (member == nullptr)
        {
            return malformed("cw: invalid member");
        }

        const std::string_view key{member->key, member->key_length};

        if (key == "markov_on_off")
        {
            if (has_gate)
            {
                return malformed("cw: duplicate markov_on_off field");
            }

            has_gate = true;
        }
        else if (key == "pulse_period_seconds" || key == "pulse_duty_cycle")
        {
            has_pulse = true;
        }

        child = member->next_sibling;
    }

    if (!has_gate)
    {
        params.markov = MarkovGateParams{};
        return m110::Status::success();
    }

    if (has_pulse)
    {
        return malformed("cw.markov_on_off cannot combine with periodic pulse fields");
    }

    const json::Value* gate = document.find(object, "markov_on_off");

    if (gate == nullptr || gate->type != json::Type::object)
    {
        return malformed("cw.markov_on_off must be an object");
    }

    return parse_gate(document, *gate, params);
}

} // namespace signal_lab
