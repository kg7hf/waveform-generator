// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// Scenario decoding
// Guide: docs/modules/signal-lab-engine.md
// Ownership: One fixed JSON workspace is guarded against concurrent parse calls.
// Contract: Failure is a Status with a static message; never parse on an audio deadline.

#include "signal_lab/scenario.hpp"

#include "cw_fading_scenario.hpp"
#include "cw_fsk_scenario.hpp"
#include "cw_markov_scenario.hpp"
#include "cw_pulse_scenario.hpp"
#include "cw_sweep_scenario.hpp"
#include "frequency_shift_scenario.hpp"
#include "second_source_scenario.hpp"
#include "signal_lab/json.hpp"
#include "watterson_scenario.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <memory>
#include <string_view>

namespace signal_lab
{
namespace
{

using json::Document;
using json::Type;
using json::Value;

// The reader is large (about 60 KB); keep one static instance instead of a stack copy.
Document scenario_document{};
std::atomic_flag scenario_parser_busy = ATOMIC_FLAG_INIT;

// A document the grammar refuses: invalid_argument with the static message.
[[nodiscard]] m110::Status malformed(const char* message) noexcept
{
    return m110::Status{.code = m110::StatusCode::invalid_argument, .message = message};
}

// The first of `reads` that failed, or success. A braced list is evaluated in
// order, so every read runs and the earliest failure is the one reported, as
// a chain of reads that stopped at it would report it; a failed scenario is
// discarded whole.
[[nodiscard]] m110::Status first_failure(std::initializer_list<m110::Status> reads) noexcept
{
    const auto failed = std::ranges::find_if(reads, [](const m110::Status& read) { return !read.is_ok(); });

    return failed == reads.end() ? m110::Status::success() : *failed;
}

[[nodiscard]] m110::Status read_number(const Document& document, const Value& object, const char* key, double& out, bool required) noexcept
{
    const Value* node = document.find(object, key);

    if (node == nullptr)
    {
        return required ? malformed("missing required numeric parameter") : m110::Status::success();
    }

    if (node->type != Type::number)
    {
        return malformed("parameter must be a number");
    }

    out = node->number;
    return m110::Status::success();
}

[[nodiscard]] m110::Status read_integer(const Document& document, const Value& object, const char* key, std::uint32_t& out, std::uint32_t low, std::uint32_t high, const char* message) noexcept
{
    double value = 0.0;

    if (const m110::Status read = read_number(document, object, key, value, true); !read.is_ok())
    {
        return read;
    }

    if (!(value >= static_cast<double>(low) && value <= static_cast<double>(high)))
    {
        return malformed(message);
    }

    out = static_cast<std::uint32_t>(value);

    if (static_cast<double>(out) != value)
    {
        return malformed(message);
    }

    return m110::Status::success();
}

[[nodiscard]] m110::Status read_window(const Document& document, const Value& object, Window& window) noexcept
{
    if (const m110::Status start = read_number(document, object, "start_seconds", window.start_seconds, false); !start.is_ok())
    {
        return start;
    }

    if (document.has(object, "duration_seconds"))
    {
        window.has_duration = true;
        return read_number(document, object, "duration_seconds", window.duration_seconds, true);
    }

    return m110::Status::success();
}

[[nodiscard]] m110::Status parse_stage_stream_id(const Document& document, const Value& object, std::uint32_t& stream_id) noexcept
{
    std::uint32_t occurrences = 0U;
    std::uint32_t child = object.first_child;

    for (std::uint32_t index = 0U; index < object.child_count; ++index)
    {
        const Value* member = document.child(child);

        if (member == nullptr)
        {
            return malformed("impairment has an invalid member");
        }

        occurrences += std::string_view{member->key, member->key_length} == "stream_id" ? 1U : 0U;
        child = member->next_sibling;
    }

    if (occurrences > 1U)
    {
        return malformed("duplicate impairment stream_id");
    }

    const Value* value = document.find(object, "stream_id");

    if (value == nullptr)
    {
        return m110::Status::success();
    }

    if (value->type != Type::number || !std::isfinite(value->number) || value->number < 1.0 || value->number > static_cast<double>(std::numeric_limits<std::uint32_t>::max()) ||
        std::trunc(value->number) != value->number)
    {
        return malformed("impairment stream_id must be an integer in 1..4294967295");
    }

    stream_id = static_cast<std::uint32_t>(value->number);
    return m110::Status::success();
}

[[nodiscard]] m110::Status parse_awgn(const Document& document, const Value& object, AwgnParams& params) noexcept
{
    double snr_db = 0.0;

    if (const m110::Status snr = read_number(document, object, "snr_db", snr_db, true); !snr.is_ok())
    {
        return snr;
    }

    params.snr = units::Ratio{snr_db, units::dB};
    return read_window(document, object, params.window);
}

[[nodiscard]] m110::Status parse_cw(const Document& document, const Value& object, CwParams& params) noexcept
{
    // Exactly one fixed or swept frequency is required; a CW may use either a
    // frame-aligned periodic pulse schedule or a Markov ON/OFF gate.
    if (const m110::Status frequency = parse_cw_frequency(document, object, params); !frequency.is_ok())
    {
        return frequency;
    }

    if (const m110::Status fsk = parse_cw_fsk(document, object, params); !fsk.is_ok())
    {
        return fsk;
    }

    if (const m110::Status fading = parse_cw_fading(document, object, params); !fading.is_ok())
    {
        return fading;
    }

    if (const m110::Status read = first_failure(
            {read_number(document, object, "ci_db", params.ci_db, true), read_number(document, object, "ramp_seconds", params.ramp_seconds, false), read_window(document, object, params.window)});
        !read.is_ok())
    {
        return read;
    }

    const Value* phase = document.find(object, "phase_degrees");

    if (phase != nullptr)
    {
        if (phase->type == Type::string && phase->equals("random"))
        {
            params.random_phase = true;
        }
        else if (phase->type == Type::number)
        {
            params.phase_degrees = phase->number;
        }
        else
        {
            return malformed(R"(cw.phase_degrees must be a number or "random")");
        }
    }

    if (const m110::Status pulse = parse_cw_pulse_schedule(document, object, params); !pulse.is_ok())
    {
        return pulse;
    }

    return parse_cw_markov_gate(document, object, params);
}

[[nodiscard]] m110::Status parse_impulse(const Document& document, const Value& object, ImpulseParams& params) noexcept
{
    if (const m110::Status read = first_failure({read_number(document, object, "peak_db", params.peak_db, true), read_number(document, object, "decay_ms", params.decay_ms, true),
                                                 read_number(document, object, "ring_hz", params.ring_hz, false), read_number(document, object, "truncate_tau", params.truncate_tau, false),
                                                 read_window(document, object, params.window)});
        !read.is_ok())
    {
        return read;
    }

    const Value* ring = document.find(object, "ring");

    if (ring != nullptr)
    {
        if (ring->equals("noise"))
        {
            params.ring_noise = true;
        }
        else if (!ring->equals("tone"))
        {
            return malformed(R"(impulse.ring must be "tone" or "noise")");
        }
    }

    const Value* phase = document.find(object, "phase_degrees");

    if (phase != nullptr)
    {
        if (phase->type == Type::number)
        {
            params.random_phase = false;
            params.phase_degrees = phase->number;
        }
        else if (!(phase->type == Type::string && phase->equals("random")))
        {
            return malformed(R"(impulse.phase_degrees must be a number or "random")");
        }
    }

    const bool has_rate = document.has(object, "rate_per_sec");
    const bool has_period = document.has(object, "period_seconds");

    if (has_rate == has_period)
    {
        return malformed("impulse needs exactly one of rate_per_sec or period_seconds");
    }

    // Exactly one of the two is present (above).
    params.poisson = has_rate;
    const m110::Status schedule = has_rate ? read_number(document, object, "rate_per_sec", params.rate_per_sec, true) : read_number(document, object, "period_seconds", params.period_seconds, true);

    if (!schedule.is_ok())
    {
        return schedule;
    }

    if (document.has(object, "first_seconds"))
    {
        params.has_first = true;

        if (const m110::Status first = read_number(document, object, "first_seconds", params.first_seconds, true); !first.is_ok())
        {
            return first;
        }
    }

    if (params.decay_ms <= 0.0 || params.truncate_tau <= 0.0 || (params.poisson && params.rate_per_sec <= 0.0) || (!params.poisson && params.period_seconds <= 0.0))
    {
        return malformed("impulse parameters out of range");
    }

    return m110::Status::success();
}

[[nodiscard]] m110::Status parse_fade(const Document& document, const Value& object, FadeParams& params) noexcept
{
    if (const m110::Status read = first_failure({read_number(document, object, "depth_db", params.depth_db, true), read_window(document, object, params.window)}); !read.is_ok())
    {
        return read;
    }

    const Value* shape = document.find(object, "shape");

    if (shape != nullptr)
    {
        if (shape->equals("rectangular"))
        {
            params.rectangular = true;
        }
        else if (!shape->equals("raised_cosine"))
        {
            return malformed("fade.shape must be raised_cosine or rectangular");
        }
    }

    params.explicit_shape = document.has(object, "attack_ms") || document.has(object, "hold_ms") || document.has(object, "recovery_ms");
    const m110::Status lengths = params.explicit_shape
                                     ? first_failure({read_number(document, object, "attack_ms", params.attack_ms, false), read_number(document, object, "hold_ms", params.hold_ms, false),
                                                      read_number(document, object, "recovery_ms", params.recovery_ms, false)})
                                     : read_number(document, object, "duration_ms", params.duration_ms, true);

    if (!lengths.is_ok())
    {
        return lengths;
    }

    const Value* starts = document.find(object, "starts_seconds");

    if (starts != nullptr)
    {
        if (starts->type != Type::array || starts->child_count > max_events)
        {
            return malformed("fade.starts_seconds must be an array of at most 32 numbers");
        }

        params.schedule = FadeSchedule::explicit_starts;

        for (std::uint32_t index = 0U; index < starts->child_count; ++index)
        {
            const Value* item = document.at(*starts, index);

            if (item == nullptr || item->type != Type::number)
            {
                return malformed("fade.starts_seconds entries must be numbers");
            }

            params.starts_seconds[index] = item->number;
        }

        params.start_count = starts->child_count;

        // Keep the list sorted (Python sorts it) with a tiny insertion sort.
        for (std::uint32_t i = 1U; i < params.start_count; ++i)
        {
            const double value = params.starts_seconds[i];
            std::uint32_t j = i;

            while (j > 0U && params.starts_seconds[j - 1U] > value)
            {
                params.starts_seconds[j] = params.starts_seconds[j - 1U];
                --j;
            }

            params.starts_seconds[j] = value;
        }
    }
    else if (document.has(object, "rate_per_sec"))
    {
        params.schedule = FadeSchedule::poisson;
        return read_number(document, object, "rate_per_sec", params.rate_per_sec, true);
    }
    else
    {
        params.schedule = FadeSchedule::periodic;

        if (const m110::Status period = read_number(document, object, "period_seconds", params.period_seconds, true); !period.is_ok())
        {
            return period;
        }

        if (document.has(object, "first_seconds"))
        {
            params.has_first = true;

            if (const m110::Status first = read_number(document, object, "first_seconds", params.first_seconds, true); !first.is_ok())
            {
                return first;
            }
        }

        if (document.has(object, "count"))
        {
            params.has_count = true;
            return read_integer(document, object, "count", params.count, 1U, std::numeric_limits<std::uint32_t>::max(), "fade.count must be an integer in 1..4294967295");
        }
    }

    return m110::Status::success();
}

[[nodiscard]] m110::Status parse_slip(const Document& document, const Value& object, SampleSlipParams& params) noexcept
{
    const auto read_length = [&](const Value& value, std::uint32_t& out) noexcept
    { return read_integer(document, value, "length_samples", out, 1U, max_slip_length, "sample_slip length_samples must be an integer in 1..1024"); };

    const Value* events = document.find(object, "events");

    if (events != nullptr)
    {
        if (events->type != Type::array || events->child_count > max_events)
        {
            return malformed("sample_slip.events must be an array of at most 32 objects");
        }

        for (std::uint32_t index = 0U; index < events->child_count; ++index)
        {
            const Value* item = document.at(*events, index);

            if (item == nullptr || item->type != Type::object)
            {
                return malformed("sample_slip.events entries must be objects");
            }

            SlipEvent& event = params.events[index];

            if (const m110::Status read = first_failure({read_number(document, *item, "at_seconds", event.at_seconds, true), read_length(*item, event.length_samples)}); !read.is_ok())
            {
                return read;
            }

            const Value* kind = document.find(*item, "kind");

            if (kind == nullptr || !(kind->equals("delete") || kind->equals("duplicate")))
            {
                return malformed("sample_slip kind must be delete or duplicate");
            }

            event.duplicate = kind->equals("duplicate");
        }

        params.event_count = events->child_count;
    }
    else
    {
        const Value* kind = document.find(object, "kind");

        if (kind == nullptr || !(kind->equals("delete") || kind->equals("duplicate")))
        {
            return malformed("sample_slip kind must be delete or duplicate");
        }

        std::uint32_t length = 0U;

        if (const m110::Status read = read_length(object, length); !read.is_ok())
        {
            return read;
        }

        const Value* placement = document.find(object, "placement");
        double first = 0.0;
        double step = 0.0;
        std::uint32_t count = 1U;
        m110::Status read{};

        if (placement == nullptr || placement->equals("single"))
        {
            read = read_number(document, object, "at_seconds", first, true);
        }
        else if (placement->equals("spaced") || placement->equals("clustered"))
        {
            const char* const step_key = placement->equals("spaced") ? "period_seconds" : "spacing_seconds";
            read = first_failure({read_number(document, object, "first_seconds", first, true), read_number(document, object, step_key, step, true),
                                  read_integer(document, object, "count", count, 1U, max_events, "sample_slip.count must be an integer in 1..32")});
        }
        else
        {
            read = malformed("sample_slip.placement must be single, spaced or clustered");
        }

        if (!read.is_ok())
        {
            return read;
        }

        params.event_count = count;

        for (std::uint32_t index = 0U; index < params.event_count; ++index)
        {
            params.events[index].at_seconds = first + static_cast<double>(index) * step;
            params.events[index].duplicate = kind->equals("duplicate");
            params.events[index].length_samples = length;
        }
    }

    return m110::Status::success();
}

// One scenario-wide AWGN/SNR band. Existing documents retain their 3 kHz band.
[[nodiscard]] m110::Status read_noise_bandwidth(const Document& document, const Value& root, NoiseBandwidth& bandwidth) noexcept
{
    constexpr std::array supported{NoiseBandwidth::khz3, NoiseBandwidth::khz6, NoiseBandwidth::khz9, NoiseBandwidth::khz12};
    std::uint32_t child = root.first_child;
    std::uint32_t occurrences = 0U;

    for (std::uint32_t index = 0U; index < root.child_count; ++index)
    {
        const Value* member = document.child(child);

        if (member == nullptr)
        {
            return malformed("invalid scenario member");
        }

        occurrences += std::string_view{member->key, member->key_length} == "noise_bandwidth_hz" ? 1U : 0U;
        child = member->next_sibling;
    }

    if (occurrences > 1U)
    {
        return malformed("duplicate noise_bandwidth_hz");
    }

    double hertz = noise_band_width(bandwidth).hertz();

    if (const auto read = read_number(document, root, "noise_bandwidth_hz", hertz, false); !read.is_ok())
    {
        return read;
    }

    for (const NoiseBandwidth candidate : supported)
    {
        if (hertz == noise_band_width(candidate).hertz())
        {
            bandwidth = candidate;
            return m110::Status::success();
        }
    }

    return malformed("noise_bandwidth_hz must be 3000, 6000, 9000 or 12000");
}

// The stages whose rules live in their own files (watterson.cpp; design 9.3), so this
// file's dispatcher keeps one fallback branch for all of them.
[[nodiscard]] m110::Status parse_typed_stage(const Document& document, const Value& item, const Value& type, Scenario& scenario, Stage& stage) noexcept
{
    if (type.equals("frequency_shift"))
    {
        stage.type = StageType::frequency_shift;
        return parse_frequency_shift_stage(document, item, scenario.frequency_shift);
    }

    if (type.equals("second_source"))
    {
        stage.type = StageType::second_source;
        return parse_second_source_stage(document, item, scenario.second_source, scenario.second_source_path);
    }

    if (!type.equals("watterson"))
    {
        return malformed("unknown impairment type");
    }

    stage.type = StageType::watterson;
    return parse_watterson_stage(document, item, scenario.watterson);
}

[[nodiscard]] m110::Status parse_stage(const Document& document, const Value& item, Scenario& scenario, Stage& stage) noexcept
{
    const Value* type = document.find(item, "type");

    if (type == nullptr)
    {
        return malformed("impairment.type is required");
    }

    if (const m110::Status stream_id = parse_stage_stream_id(document, item, stage.stream_id); !stream_id.is_ok())
    {
        return stream_id;
    }

    if (type->equals("awgn"))
    {
        stage.type = StageType::awgn;
        return parse_awgn(document, item, stage.awgn);
    }

    if (type->equals("cw"))
    {
        stage.type = StageType::cw;
        return parse_cw(document, item, stage.cw);
    }

    if (type->equals("impulse"))
    {
        stage.type = StageType::impulse;
        return parse_impulse(document, item, stage.impulse);
    }

    if (type->equals("fade"))
    {
        stage.type = StageType::fade;
        return parse_fade(document, item, stage.fade);
    }

    if (type->equals("sample_slip"))
    {
        stage.type = StageType::sample_slip;
        return parse_slip(document, item, stage.slip);
    }

    return parse_typed_stage(document, item, *type, scenario, stage);
}

} // namespace

const char* stage_type_name(StageType type) noexcept
{
    switch (type)
    {
    case StageType::awgn:
        return "awgn";

    case StageType::cw:
        return "cw";

    case StageType::impulse:
        return "impulse";

    case StageType::fade:
        return "fade";

    case StageType::sample_slip:
        return "sample_slip";

    case StageType::watterson:
        return "watterson";

    case StageType::frequency_shift:
        return "frequency_shift";

    case StageType::second_source:
        return "second_source";
    }

    return "?";
}

m110::Status parse_scenario(const char* text, std::size_t length, Scenario& scenario) noexcept
{
    if (scenario_parser_busy.test_and_set())
    {
        return m110::Status{.code = m110::StatusCode::busy, .message = "scenario parser is busy"};
    }

    struct ParserRelease
    {
        ParserRelease() = default;
        ParserRelease(const ParserRelease&) = delete;
        ParserRelease& operator=(const ParserRelease&) = delete;
        ParserRelease(ParserRelease&&) = delete;
        ParserRelease& operator=(ParserRelease&&) = delete;
        ~ParserRelease() noexcept { scenario_parser_busy.clear(); }
    } release;

    // Construct the default value in its existing storage. Assigning a large
    // aggregate temporary puts the entire stage array on the player stack.
    std::destroy_at(std::addressof(scenario));
    std::construct_at(std::addressof(scenario));
    Document& document = scenario_document;

    if (const m110::Status parsed = document.parse(text, length); !parsed.is_ok())
    {
        return parsed;
    }

    const Value* root = document.root();

    if (root == nullptr || root->type != Type::object)
    {
        return malformed("scenario must be a JSON object");
    }

    const Value* schema = document.find(*root, "schema");

    if (schema != nullptr && !schema->equals("signal-lab.scenario/1"))
    {
        return malformed("unsupported scenario schema");
    }

    const Value* test_id = document.find(*root, "test_id");

    if (test_id != nullptr && test_id->type == Type::string)
    {
        std::uint32_t copy = test_id->text_length < max_test_id - 1U ? test_id->text_length : max_test_id - 1U;

        for (std::uint32_t index = 0U; index < copy; ++index)
        {
            scenario.test_id[index] = test_id->text[index];
        }

        scenario.test_id[copy] = '\0';
    }

    const Value* seed = document.find(*root, "seed");

    if (seed != nullptr)
    {
        if (seed->type != Type::number || !std::isfinite(seed->number) || seed->number < 0.0 || seed->number >= 18446744073709551616.0 || std::trunc(seed->number) != seed->number)
        {
            return malformed("seed must be a non-negative integer");
        }

        scenario.seed = static_cast<std::uint64_t>(seed->number);
    }

    if (const m110::Status settings = first_failure({read_number(document, *root, "source_gain_db", scenario.source_gain_db, false), read_noise_bandwidth(document, *root, scenario.noise_bandwidth)});
        !settings.is_ok())
    {
        return settings;
    }

    const Value* clip = document.find(*root, "clip_policy");

    if (clip != nullptr)
    {
        if (clip->equals("reject"))
        {
            scenario.saturate = false;
        }
        else if (!clip->equals("saturate"))
        {
            return malformed("clip_policy must be saturate or reject");
        }
    }

    const Value* reference = document.find(*root, "reference");

    if (reference != nullptr && !(reference->type == Type::string && reference->equals("auto")))
    {
        if (reference->type != Type::object)
        {
            return malformed(R"(reference must be "auto" or an interval object)");
        }

        scenario.reference_auto = false;

        if (const m110::Status interval = first_failure({read_number(document, *reference, "start_seconds", scenario.reference_start_seconds, true),
                                                         read_number(document, *reference, "duration_seconds", scenario.reference_duration_seconds, true)});
            !interval.is_ok())
        {
            return interval;
        }
    }

    if (document.has(*root, "reference_rms"))
    {
        scenario.has_reference_rms = true;

        if (!read_number(document, *root, "reference_rms", scenario.reference_rms, true).is_ok() || scenario.reference_rms <= 0.0)
        {
            return malformed("reference_rms must be a positive number");
        }
    }

    const Value* impairments = document.find(*root, "impairments");

    if (impairments == nullptr)
    {
        return validate_scenario(scenario);
    }

    if (impairments->type != Type::array)
    {
        return malformed("impairments must be an array");
    }

    if (impairments->child_count > max_stages)
    {
        return malformed("too many impairment stages (maximum 9)");
    }

    for (std::uint32_t index = 0U; index < impairments->child_count; ++index)
    {
        const Value* item = document.at(*impairments, index);

        if (item == nullptr || item->type != Type::object)
        {
            return malformed("each impairment must be an object");
        }

        Stage& stage = scenario.stages[index];

        if (const m110::Status parsed = parse_stage(document, *item, scenario, stage); !parsed.is_ok())
        {
            return parsed;
        }

        scenario.stage_count = index + 1U;
    }

    for (std::uint32_t index = 0U; index + 1U < scenario.stage_count; ++index)
    {
        if (scenario.stages[index].type == StageType::sample_slip)
        {
            return malformed("sample_slip must be the last impairment");
        }
    }

    return validate_scenario(scenario);
}

} // namespace signal_lab
