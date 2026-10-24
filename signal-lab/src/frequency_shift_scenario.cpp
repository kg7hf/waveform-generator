// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// Whole-signal frequency shift scenario boundary.
// Guide: docs/modules/signal-lab-engine.md.

#include "frequency_shift_scenario.hpp"
#include "signal_lab/frequency_shift.hpp"

#include <algorithm>
#include <array>
#include <span>
#include <string_view>

namespace signal_lab
{
namespace
{

[[nodiscard]] m110::Status invalid(const char* message) noexcept
{
    return {m110::StatusCode::invalid_configuration, message};
}

[[nodiscard]] m110::Status check_members(const json::Document& document, const json::Value& object, FrequencyShiftMode mode) noexcept
{
    constexpr std::array offset_keys{std::string_view{"type"}, std::string_view{"mode"}, std::string_view{"offset_hz"}, std::string_view{"stream_id"}};
    constexpr std::array sweep_keys{
        std::string_view{"type"},      std::string_view{"mode"},     std::string_view{"amplitude_hz"}, std::string_view{"slope_hz_per_second"}, std::string_view{"start_frequency_hz"},
        std::string_view{"direction"}, std::string_view{"stream_id"}};
    const std::span<const std::string_view> keys = mode == FrequencyShiftMode::offset ? std::span<const std::string_view>{offset_keys} : std::span<const std::string_view>{sweep_keys};
    std::uint32_t seen = 0U;
    std::uint32_t child = object.first_child;

    for (std::uint32_t index = 0U; index < object.child_count; ++index)
    {
        const json::Value* member = document.child(child);

        if (member == nullptr)
        {
            return invalid("frequency_shift: invalid member");
        }

        const auto found = std::ranges::find(keys, std::string_view{member->key, member->key_length});

        if (found == keys.end())
        {
            return invalid("frequency_shift: unknown or mode-inapplicable member");
        }

        const auto bit = std::uint32_t{1U} << static_cast<std::uint32_t>(found - keys.begin());

        if ((seen & bit) != 0U)
        {
            return invalid("frequency_shift: duplicate member");
        }

        seen |= bit;
        child = member->next_sibling;
    }

    return m110::Status::success();
}

[[nodiscard]] m110::Result<double> number(const json::Document& document, const json::Value& object, const char* key, double fallback) noexcept
{
    const json::Value* value = document.find(object, key);

    if (value == nullptr)
    {
        return fallback;
    }

    return value->type == json::Type::number ? m110::Result<double>{value->number} : m110::Result<double>{invalid("frequency_shift: expected numeric parameter")};
}

[[nodiscard]] m110::Status read_sweep(const json::Document& document, const json::Value& object, FrequencyShiftParams& params) noexcept
{
    const auto amplitude = number(document, object, "amplitude_hz", params.amplitude.hertz());
    const auto slope = number(document, object, "slope_hz_per_second", params.slope.hertz_per_second());

    if (!amplitude.has_value())
    {
        return amplitude.status();
    }

    if (!slope.has_value())
    {
        return slope.status();
    }

    // Changing the amplitude also changes the default starting point.
    const auto start = number(document, object, "start_frequency_hz", -amplitude.value());

    if (!start.has_value())
    {
        return start.status();
    }

    params.amplitude = units::FrequencyDelta{amplitude.value(), units::Hz};
    params.slope = FrequencySlope{slope.value()};
    params.start_frequency = units::FrequencyDelta{start.value(), units::Hz};
    const json::Value* direction = document.find(object, "direction");

    if (direction != nullptr)
    {
        if (direction->equals("ascending"))
        {
            params.direction = TriangleDirection::ascending;
        }
        else if (direction->equals("descending"))
        {
            params.direction = TriangleDirection::descending;
        }
        else
        {
            return invalid("frequency_shift: direction must be ascending or descending");
        }
    }

    return validate_frequency_shift_params(params);
}

} // namespace

m110::Status parse_frequency_shift_stage(const json::Document& document, const json::Value& object, FrequencyShiftParams& params) noexcept
{
    params = FrequencyShiftParams{};
    const json::Value* mode = document.find(object, "mode");

    if (mode == nullptr || (!mode->equals("offset") && !mode->equals("sweep")))
    {
        return invalid("frequency_shift: mode must be offset or sweep");
    }

    params.mode = mode->equals("offset") ? FrequencyShiftMode::offset : FrequencyShiftMode::sweep;

    if (const auto checked = check_members(document, object, params.mode); !checked.is_ok())
    {
        return checked;
    }

    if (params.mode == FrequencyShiftMode::sweep)
    {
        return read_sweep(document, object, params);
    }

    const json::Value* offset = document.find(object, "offset_hz");

    if (offset == nullptr || offset->type != json::Type::number)
    {
        return invalid("frequency_shift: offset mode requires numeric offset_hz");
    }

    params.offset = units::FrequencyDelta{offset->number, units::Hz};
    return validate_frequency_shift_params(params);
}

m110::Status validate_frequency_shift_stage(const Scenario& scenario, std::uint32_t index) noexcept
{
    std::uint32_t count = 0U;

    for (const Stage& stage : std::span{scenario.stages}.first(scenario.stage_count))
    {
        count += stage.type == StageType::frequency_shift ? 1U : 0U;
    }

    if (count != 1U)
    {
        return invalid("frequency_shift: at most one stage per scenario");
    }

    const std::uint32_t position = scenario.stages[0].type == StageType::watterson ? 1U : 0U;

    if (index != position)
    {
        return invalid("frequency_shift: must follow Watterson or be first, before noise and interference");
    }

    return validate_frequency_shift_params(scenario.frequency_shift);
}

} // namespace signal_lab
