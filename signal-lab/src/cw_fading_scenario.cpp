// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// Strict bounded CW-fading scenario parsing.

#include "cw_fading_scenario.hpp"

#include "signal_lab/cw_fading.hpp"

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

[[nodiscard]] m110::Status parse_fading(const json::Document& document, const json::Value& object, CwParams& params) noexcept
{
    std::uint32_t child = object.first_child;
    bool found_spread = false;

    for (std::uint32_t index = 0U; index < object.child_count; ++index)
    {
        const json::Value* member = document.child(child);

        if (member == nullptr)
        {
            return malformed("cw.fading: invalid member");
        }

        if (std::string_view{member->key, member->key_length} != "spread_hz")
        {
            return malformed("cw.fading: unknown member");
        }

        if (found_spread)
        {
            return malformed("cw.fading: duplicate spread_hz");
        }

        found_spread = true;
        child = member->next_sibling;
    }

    if (!found_spread)
    {
        return malformed("cw.fading requires spread_hz");
    }

    const json::Value* spread = document.find(object, "spread_hz");

    if (spread == nullptr || spread->type != json::Type::number || !std::isfinite(spread->number))
    {
        return malformed("cw.fading.spread_hz must be a finite number");
    }

    params.fading = CwFadingParams{
        .enabled = true,
        .spread = units::FrequencyDelta::from_hz(spread->number),
    };
    return validate_cw_fading_params(params.fading);
}

} // namespace

m110::Status parse_cw_fading(const json::Document& document, const json::Value& object, CwParams& params) noexcept
{
    bool has_fading = false;
    std::uint32_t child = object.first_child;

    for (std::uint32_t index = 0U; index < object.child_count; ++index)
    {
        const json::Value* member = document.child(child);

        if (member == nullptr)
        {
            return malformed("cw: invalid member");
        }

        if (std::string_view{member->key, member->key_length} == "fading")
        {
            if (has_fading)
            {
                return malformed("cw: duplicate fading field");
            }

            has_fading = true;
        }

        child = member->next_sibling;
    }

    if (!has_fading)
    {
        params.fading = CwFadingParams{};
        return m110::Status::success();
    }

    const json::Value* fading = document.find(object, "fading");

    if (fading == nullptr || fading->type != json::Type::object)
    {
        return malformed("cw.fading must be an object");
    }

    return parse_fading(document, *fading, params);
}

} // namespace signal_lab
