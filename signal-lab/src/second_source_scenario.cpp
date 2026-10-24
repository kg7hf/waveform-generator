// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// Second-source scenario grammar and stage placement.
// Guide: docs/modules/second-source.md.

#include "second_source_scenario.hpp"

#include "signal_lab/second_source_params.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <iterator>
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

[[nodiscard]] m110::Status malformed(const char* message) noexcept
{
    return {m110::StatusCode::invalid_argument, message};
}

[[nodiscard]] bool valid_utf8(std::string_view text) noexcept
{
    std::size_t index = 0U;

    while (index < text.size())
    {
        const auto first = static_cast<unsigned char>(text[index]);

        if (first < 0x80U)
        {
            if (first < 0x20U || first == 0x7FU)
            {
                return false;
            }

            ++index;
            continue;
        }

        std::size_t continuation_count = 0U;
        unsigned char second_low = 0x80U;
        unsigned char second_high = 0xBFU;

        if (first >= 0xC2U && first <= 0xDFU)
        {
            continuation_count = 1U;
        }
        else if (first == 0xE0U)
        {
            continuation_count = 2U;
            second_low = 0xA0U;
        }
        else if ((first >= 0xE1U && first <= 0xECU) || (first >= 0xEEU && first <= 0xEFU))
        {
            continuation_count = 2U;
        }
        else if (first == 0xEDU)
        {
            continuation_count = 2U;
            second_high = 0x9FU;
        }
        else if (first == 0xF0U)
        {
            continuation_count = 3U;
            second_low = 0x90U;
        }
        else if (first >= 0xF1U && first <= 0xF3U)
        {
            continuation_count = 3U;
        }
        else if (first == 0xF4U)
        {
            continuation_count = 3U;
            second_high = 0x8FU;
        }
        else
        {
            return false;
        }

        if (index + continuation_count >= text.size())
        {
            return false;
        }

        const auto second = static_cast<unsigned char>(text[index + 1U]);

        if (second < second_low || second > second_high)
        {
            return false;
        }

        for (std::size_t offset = 2U; offset <= continuation_count; ++offset)
        {
            const auto continuation = static_cast<unsigned char>(text[index + offset]);

            if (continuation < 0x80U || continuation > 0xBFU)
            {
                return false;
            }
        }

        index += continuation_count + 1U;
    }

    return true;
}

[[nodiscard]] m110::Status check_members(const json::Document& document, const json::Value& object) noexcept
{
    constexpr std::array keys{std::string_view{"type"},        std::string_view{"stream_id"},          std::string_view{"source_path"},     std::string_view{"si_db"},
                              std::string_view{"start_frame"}, std::string_view{"source_start_frame"}, std::string_view{"duration_frames"}, std::string_view{"frequency_offset_hz"},
                              std::string_view{"fading"},      std::string_view{"markov_on_off"}};
    std::uint32_t seen = 0U;
    std::uint32_t child = object.first_child;

    for (std::uint32_t index = 0U; index < object.child_count; ++index)
    {
        const json::Value* member = document.child(child);

        if (member == nullptr)
        {
            return malformed("second_source: invalid member");
        }

        const auto found = std::ranges::find(keys, std::string_view{member->key, member->key_length});

        if (found == keys.end())
        {
            return malformed("second_source: unknown member");
        }

        const auto bit = std::uint32_t{1U} << static_cast<std::uint32_t>(found - keys.begin());

        if ((seen & bit) != 0U)
        {
            return malformed("second_source: duplicate member");
        }

        seen |= bit;
        child = member->next_sibling;
    }

    return m110::Status::success();
}

[[nodiscard]] m110::Result<double> number(const json::Document& document, const json::Value& object, const char* key, double fallback, bool required) noexcept
{
    const json::Value* value = document.find(object, key);

    if (value == nullptr)
    {
        return required ? m110::Result<double>{malformed("second_source: missing required numeric member")} : m110::Result<double>{fallback};
    }

    return value->type == json::Type::number ? m110::Result<double>{value->number} : m110::Result<double>{malformed("second_source: numeric members must be numbers")};
}

[[nodiscard]] m110::Result<std::uint64_t> frame_number(const json::Document& document, const json::Value& object, const char* key, std::uint64_t fallback, bool required, bool allow_zero) noexcept
{
    if (!document.has(object, key) && !required)
    {
        return fallback;
    }

    const auto value = number(document, object, key, static_cast<double>(fallback), required);

    if (!value.has_value())
    {
        return value.status();
    }

    const double parsed = value.value();
    const double minimum = allow_zero ? 0.0 : 1.0;

    if (!std::isfinite(parsed) || parsed < minimum || parsed > static_cast<double>(max_second_source_frames) || std::trunc(parsed) != parsed)
    {
        return malformed(allow_zero ? "second_source frame values must be whole frames in range" : "second_source duration_frames must be a positive whole-frame count");
    }

    return static_cast<std::uint64_t>(parsed);
}

[[nodiscard]] m110::Status read_frames(const json::Document& document, const json::Value& object, SecondSourceParams& params) noexcept
{
    const auto start = frame_number(document, object, "start_frame", 0U, false, true);
    const auto source_start = frame_number(document, object, "source_start_frame", 0U, false, true);
    const auto duration = frame_number(document, object, "duration_frames", 0U, false, false);

    if (!start.has_value())
    {
        return start.status();
    }

    if (!source_start.has_value())
    {
        return source_start.status();
    }

    if (!duration.has_value())
    {
        return duration.status();
    }

    params.start_frame = start.value();
    params.source_start_frame = source_start.value();
    params.duration_frames = duration.value();
    return m110::Status::success();
}

[[nodiscard]] m110::Status read_fading(const json::Document& document, const json::Value& object, SecondSourceParams& params) noexcept
{
    const json::Value* fading = document.find(object, "fading");

    if (fading == nullptr)
    {
        return m110::Status::success();
    }

    if (fading->type != json::Type::object || fading->child_count != 1U)
    {
        return malformed("second_source.fading must contain exactly one spread_hz number");
    }

    const json::Value* spread = document.child(fading->first_child);

    if (spread == nullptr || std::string_view{spread->key, spread->key_length} != "spread_hz" || spread->type != json::Type::number || !std::isfinite(spread->number))
    {
        return malformed("second_source.fading.spread_hz must be a finite number");
    }

    params.fading.enabled = true;
    params.fading.spread = units::FrequencyDelta::from_hz(spread->number);
    return m110::Status::success();
}

[[nodiscard]] m110::Status read_markov_gate(const json::Document& document, const json::Value& object, SecondSourceParams& params) noexcept
{
    const json::Value* gate = document.find(object, "markov_on_off");

    if (gate == nullptr)
    {
        return m110::Status::success();
    }

    if (gate->type != json::Type::object || gate->child_count != 2U)
    {
        return malformed("second_source.markov_on_off requires exactly mean_on_seconds and mean_off_seconds");
    }

    // Exactly two members and both required names also rejects unknown and
    // duplicate keys, without accepting the reader's first-match behavior.
    const auto mean_on = number(document, *gate, "mean_on_seconds", 0.0, true);
    const auto mean_off = number(document, *gate, "mean_off_seconds", 0.0, true);

    if (!mean_on.has_value())
    {
        return mean_on.status();
    }

    if (!mean_off.has_value())
    {
        return mean_off.status();
    }

    params.markov = MarkovGateParams{.enabled = true, .mean_on_seconds = mean_on.value(), .mean_off_seconds = mean_off.value()};
    return m110::Status::success();
}

} // namespace

m110::Status parse_second_source_stage(const json::Document& document, const json::Value& object, SecondSourceParams& params, char (&source_path)[second_source_path_capacity]) noexcept
{
    if (const m110::Status checked = check_members(document, object); !checked.is_ok())
    {
        return checked;
    }

    const json::Value* path = document.find(object, "source_path");

    if (path == nullptr || path->type != json::Type::string || path->text_length == 0U || path->text_length >= second_source_path_capacity || path->text == nullptr)
    {
        return malformed("second_source source_path must be non-empty and fit its fixed buffer");
    }

    const std::string_view path_text{path->text, path->text_length};

    if (path_text.find('\0') != std::string_view::npos || path_text.find('\\') != std::string_view::npos || !valid_utf8(path_text))
    {
        return malformed("second_source source_path must be valid UTF-8 with forward slashes and no JSON escapes");
    }

    params = SecondSourceParams{};
    std::copy_n(path->text, path->text_length, source_path);
    source_path[path->text_length] = '\0';
    const auto si_db = number(document, object, "si_db", 0.0, true);
    const auto offset_hz = number(document, object, "frequency_offset_hz", 0.0, false);

    if (!si_db.has_value())
    {
        return si_db.status();
    }

    if (!offset_hz.has_value())
    {
        return offset_hz.status();
    }

    params.si = units::Ratio{si_db.value(), units::dB};
    params.frequency_offset = units::FrequencyDelta{offset_hz.value(), units::Hz};

    if (const m110::Status frames = read_frames(document, object, params); !frames.is_ok())
    {
        return frames;
    }

    if (const m110::Status fading = read_fading(document, object, params); !fading.is_ok())
    {
        return fading;
    }

    if (const m110::Status gate = read_markov_gate(document, object, params); !gate.is_ok())
    {
        return gate;
    }

    return validate_second_source_params(params);
}

m110::Status validate_second_source_stage(const Scenario& scenario, std::uint32_t index) noexcept
{
    std::uint32_t count = 0U;

    for (const Stage& stage : std::span{scenario.stages}.first(scenario.stage_count))
    {
        count += stage.type == StageType::second_source ? 1U : 0U;
    }

    if (count != 1U)
    {
        return invalid("second_source: at most one stage per scenario");
    }

    std::uint32_t expected_index = 0U;

    if (scenario.stages[expected_index].type == StageType::watterson)
    {
        ++expected_index;
    }

    if (expected_index < scenario.stage_count && scenario.stages[expected_index].type == StageType::frequency_shift)
    {
        ++expected_index;
    }

    if (index != expected_index)
    {
        return invalid("second_source: must follow Watterson/frequency_shift and precede noise or interference");
    }

    const auto terminator = std::find(std::begin(scenario.second_source_path), std::end(scenario.second_source_path), '\0');

    if (terminator == std::end(scenario.second_source_path))
    {
        return invalid("second_source: source_path must be null-terminated");
    }

    if (const m110::Status valid = validate_second_source_params(scenario.second_source); !valid.is_ok())
    {
        return valid;
    }

    if (scenario.second_source.duration_frames != 0U && (scenario.second_source.start_frame > max_second_source_frames - scenario.second_source.duration_frames ||
                                                         scenario.second_source.source_start_frame > max_second_source_frames - scenario.second_source.duration_frames))
    {
        return invalid("second_source frame window exceeds the supported frame range");
    }

    return m110::Status::success();
}

} // namespace signal_lab
