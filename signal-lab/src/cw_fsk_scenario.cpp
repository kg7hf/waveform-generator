// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// Strict bounded two-tone FSK scenario parsing.

#include "cw_fsk_scenario.hpp"

#include "signal_lab/cw_fsk.hpp"

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
        return malformed("cw.fsk numeric fields must be finite numbers");
    }

    return member->number;
}

[[nodiscard]] m110::Status parse_fsk(const json::Document& document, const json::Value& object, CwParams& params) noexcept
{
    constexpr std::array<std::string_view, 3U> names{"baud", "shift_hz", "pattern"};
    std::array<bool, names.size()> found{};
    std::uint32_t child = object.first_child;

    for (std::uint32_t index = 0U; index < object.child_count; ++index)
    {
        const json::Value* member = document.child(child);

        if (member == nullptr)
        {
            return malformed("cw.fsk: invalid member");
        }

        const std::string_view key{member->key, member->key_length};
        std::size_t name_index = 0U;

        while (name_index < names.size() && names[name_index] != key)
        {
            ++name_index;
        }

        if (name_index == names.size())
        {
            return malformed("cw.fsk: unknown member");
        }

        if (found[name_index])
        {
            return malformed("cw.fsk: duplicate member");
        }

        found[name_index] = true;
        child = member->next_sibling;
    }

    if (!found[0U] || !found[1U] || !found[2U])
    {
        return malformed("cw.fsk requires baud, shift_hz and pattern");
    }

    const auto baud = numeric_member(document, object, "baud");
    const auto shift = numeric_member(document, object, "shift_hz");
    const json::Value* pattern = document.find(object, "pattern");

    if (!baud.has_value())
    {
        return baud.status();
    }

    if (!shift.has_value())
    {
        return shift.status();
    }

    if (pattern == nullptr || pattern->type != json::Type::string)
    {
        return malformed("cw.fsk.pattern must be alternating or random");
    }

    FskPattern pattern_value{};

    if (pattern->equals("alternating"))
    {
        pattern_value = FskPattern::alternating;
    }
    else if (pattern->equals("random"))
    {
        pattern_value = FskPattern::random;
    }
    else
    {
        return malformed("cw.fsk.pattern must be alternating or random");
    }

    params.fsk = FskParams{
        .enabled = true,
        .baud = baud.value(),
        .shift = units::FrequencyDelta::from_hz(shift.value()),
        .pattern = pattern_value,
    };

    if (params.sweep.enabled)
    {
        return malformed("cw.fsk requires fixed frequency_hz and cannot combine with frequency_sweep");
    }

    return validate_cw_fsk_params(params.fsk, params.frequency_hz);
}

} // namespace

m110::Status parse_cw_fsk(const json::Document& document, const json::Value& object, CwParams& params) noexcept
{
    bool has_fsk = false;
    std::uint32_t child = object.first_child;

    for (std::uint32_t index = 0U; index < object.child_count; ++index)
    {
        const json::Value* member = document.child(child);

        if (member == nullptr)
        {
            return malformed("cw: invalid member");
        }

        if (std::string_view{member->key, member->key_length} == "fsk")
        {
            if (has_fsk)
            {
                return malformed("cw: duplicate fsk field");
            }

            has_fsk = true;
        }

        child = member->next_sibling;
    }

    if (!has_fsk)
    {
        params.fsk = FskParams{};
        return m110::Status::success();
    }

    const json::Value* fsk = document.find(object, "fsk");

    if (fsk == nullptr || fsk->type != json::Type::object)
    {
        return malformed("cw.fsk must be an object");
    }

    return parse_fsk(document, *fsk, params);
}

} // namespace signal_lab
