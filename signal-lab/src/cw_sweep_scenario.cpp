// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// Strict CW frequency selection and nested triangle-sweep scenario parsing.

#include "cw_sweep_scenario.hpp"

#include <algorithm>
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
        return malformed("cw frequency-sweep members must be finite numbers");
    }

    return member->number;
}

[[nodiscard]] m110::Status parse_sweep(const json::Document& document, const json::Value& object, CwParams& params) noexcept
{
    constexpr std::array<std::string_view, 3U> names{"low_hz", "high_hz", "slope_hz_per_second"};
    std::array<bool, names.size()> found{};
    std::uint32_t child = object.first_child;

    for (std::uint32_t index = 0U; index < object.child_count; ++index)
    {
        const json::Value* member = document.child(child);

        if (member == nullptr)
        {
            return malformed("cw.frequency_sweep: invalid member");
        }

        const std::string_view key{member->key, member->key_length};
        std::size_t name_index = 0U;

        while (name_index < names.size() && names[name_index] != key)
        {
            ++name_index;
        }

        if (name_index == names.size())
        {
            return malformed("cw.frequency_sweep: unknown member");
        }

        if (found[name_index])
        {
            return malformed("cw.frequency_sweep: duplicate member");
        }

        found[name_index] = true;
        child = member->next_sibling;
    }

    if (!std::ranges::all_of(found, [](bool member_found) { return member_found; }))
    {
        return malformed("cw.frequency_sweep requires low_hz, high_hz and slope_hz_per_second");
    }

    const auto low = numeric_member(document, object, "low_hz");
    const auto high = numeric_member(document, object, "high_hz");
    const auto slope = numeric_member(document, object, "slope_hz_per_second");

    if (!low.has_value())
    {
        return low.status();
    }

    if (!high.has_value())
    {
        return high.status();
    }

    if (!slope.has_value())
    {
        return slope.status();
    }

    params.frequency_hz = 0.0;
    params.sweep.enabled = true;
    params.sweep.low = units::Frequency::from_hz(low.value());
    params.sweep.high = units::Frequency::from_hz(high.value());
    params.sweep.slope = FrequencySlope{slope.value()};
    return validate_cw_sweep_params(params.sweep);
}

} // namespace

m110::Status parse_cw_frequency(const json::Document& document, const json::Value& object, CwParams& params) noexcept
{
    bool has_frequency = false;
    bool has_sweep = false;
    std::uint32_t child = object.first_child;

    for (std::uint32_t index = 0U; index < object.child_count; ++index)
    {
        const json::Value* member = document.child(child);

        if (member == nullptr)
        {
            return malformed("cw: invalid member");
        }

        const std::string_view key{member->key, member->key_length};
        bool* const found = key == "frequency_hz" ? &has_frequency : key == "frequency_sweep" ? &has_sweep : nullptr;

        if (found != nullptr)
        {
            if (*found)
            {
                return malformed("cw: duplicate frequency selection");
            }

            *found = true;
        }

        child = member->next_sibling;
    }

    if (has_frequency == has_sweep)
    {
        return malformed("cw requires exactly one of frequency_hz or frequency_sweep");
    }

    if (has_frequency)
    {
        const auto frequency = numeric_member(document, object, "frequency_hz");

        if (!frequency.has_value())
        {
            return frequency.status();
        }

        params.frequency_hz = frequency.value();
        params.sweep = CwSweepParams{};
        return m110::Status::success();
    }

    const json::Value* sweep = document.find(object, "frequency_sweep");

    if (sweep == nullptr || sweep->type != json::Type::object)
    {
        return malformed("cw.frequency_sweep must be an object");
    }

    return parse_sweep(document, *sweep, params);
}

} // namespace signal_lab
