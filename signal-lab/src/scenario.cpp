#include "signal_lab/scenario.hpp"

#include "signal_lab/json.hpp"

#include <limits>

namespace signal_lab
{
namespace
{

using json::Document;
using json::Type;
using json::Value;

// The reader is large (about 60 KB); keep one static instance instead of a stack copy.
Document scenario_document{};

bool set_error(const char** error, const char* message) noexcept
{
    if (error != nullptr)
    {
        *error = message;
    }
    return false;
}

bool read_number(const Document& document, const Value& object, const char* key, double& out, bool required, const char** error) noexcept
{
    const Value* node = document.find(object, key);
    if (node == nullptr)
    {
        return required ? set_error(error, "missing required numeric parameter") : true;
    }
    if (node->type != Type::number)
    {
        return set_error(error, "parameter must be a number");
    }
    out = node->number;
    return true;
}

bool read_integer(const Document& document, const Value& object, const char* key, std::uint32_t& out,
                  std::uint32_t low, std::uint32_t high, const char* message, const char** error) noexcept
{
    double value = 0.0;
    if (!read_number(document, object, key, value, true, error))
    {
        return false;
    }
    if (!(value >= static_cast<double>(low) && value <= static_cast<double>(high)))
    {
        return set_error(error, message);
    }
    out = static_cast<std::uint32_t>(value);
    if (static_cast<double>(out) != value)
    {
        return set_error(error, message);
    }
    return true;
}

bool read_window(const Document& document, const Value& object, Window& window, const char** error) noexcept
{
    if (!read_number(document, object, "start_seconds", window.start_seconds, false, error))
    {
        return false;
    }
    if (document.has(object, "duration_seconds"))
    {
        window.has_duration = true;
        if (!read_number(document, object, "duration_seconds", window.duration_seconds, true, error))
        {
            return false;
        }
    }
    return true;
}

bool parse_awgn(const Document& document, const Value& object, AwgnParams& params, const char** error) noexcept
{
    return read_number(document, object, "snr_db", params.snr_db, true, error) && read_window(document, object, params.window, error);
}

bool parse_cw(const Document& document, const Value& object, CwParams& params, const char** error) noexcept
{
    if (!read_number(document, object, "frequency_hz", params.frequency_hz, true, error) ||
        !read_number(document, object, "ci_db", params.ci_db, true, error) ||
        !read_number(document, object, "ramp_seconds", params.ramp_seconds, false, error) ||
        !read_window(document, object, params.window, error))
    {
        return false;
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
            return set_error(error, "cw.phase_degrees must be a number or \"random\"");
        }
    }
    return true;
}

bool parse_impulse(const Document& document, const Value& object, ImpulseParams& params, const char** error) noexcept
{
    if (!read_number(document, object, "peak_db", params.peak_db, true, error) ||
        !read_number(document, object, "decay_ms", params.decay_ms, true, error) ||
        !read_number(document, object, "ring_hz", params.ring_hz, false, error) ||
        !read_number(document, object, "truncate_tau", params.truncate_tau, false, error) ||
        !read_window(document, object, params.window, error))
    {
        return false;
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
            return set_error(error, "impulse.ring must be \"tone\" or \"noise\"");
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
            return set_error(error, "impulse.phase_degrees must be a number or \"random\"");
        }
    }
    const bool has_rate = document.has(object, "rate_per_sec");
    const bool has_period = document.has(object, "period_seconds");
    if (has_rate == has_period)
    {
        return set_error(error, "impulse needs exactly one of rate_per_sec or period_seconds");
    }
    params.poisson = has_rate;
    if (has_rate && !read_number(document, object, "rate_per_sec", params.rate_per_sec, true, error))
    {
        return false;
    }
    if (has_period && !read_number(document, object, "period_seconds", params.period_seconds, true, error))
    {
        return false;
    }
    if (document.has(object, "first_seconds"))
    {
        params.has_first = true;
        if (!read_number(document, object, "first_seconds", params.first_seconds, true, error))
        {
            return false;
        }
    }
    if (params.decay_ms <= 0.0 || params.truncate_tau <= 0.0 || (params.poisson && params.rate_per_sec <= 0.0) || (!params.poisson && params.period_seconds <= 0.0))
    {
        return set_error(error, "impulse parameters out of range");
    }
    return true;
}

bool parse_fade(const Document& document, const Value& object, FadeParams& params, const char** error) noexcept
{
    if (!read_number(document, object, "depth_db", params.depth_db, true, error) || !read_window(document, object, params.window, error))
    {
        return false;
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
            return set_error(error, "fade.shape must be raised_cosine or rectangular");
        }
    }
    if (document.has(object, "attack_ms") || document.has(object, "hold_ms") || document.has(object, "recovery_ms"))
    {
        params.explicit_shape = true;
        if (!read_number(document, object, "attack_ms", params.attack_ms, false, error) ||
            !read_number(document, object, "hold_ms", params.hold_ms, false, error) ||
            !read_number(document, object, "recovery_ms", params.recovery_ms, false, error))
        {
            return false;
        }
    }
    else if (!read_number(document, object, "duration_ms", params.duration_ms, true, error))
    {
        return false;
    }
    const Value* starts = document.find(object, "starts_seconds");
    if (starts != nullptr)
    {
        if (starts->type != Type::array || starts->child_count > max_events)
        {
            return set_error(error, "fade.starts_seconds must be an array of at most 32 numbers");
        }
        params.schedule = FadeSchedule::explicit_starts;
        for (std::uint32_t index = 0U; index < starts->child_count; ++index)
        {
            const Value* item = document.at(*starts, index);
            if (item == nullptr || item->type != Type::number)
            {
                return set_error(error, "fade.starts_seconds entries must be numbers");
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
        if (!read_number(document, object, "rate_per_sec", params.rate_per_sec, true, error))
        {
            return false;
        }
    }
    else
    {
        params.schedule = FadeSchedule::periodic;
        if (!read_number(document, object, "period_seconds", params.period_seconds, true, error))
        {
            return false;
        }
        if (document.has(object, "first_seconds"))
        {
            params.has_first = true;
            if (!read_number(document, object, "first_seconds", params.first_seconds, true, error))
            {
                return false;
            }
        }
        if (document.has(object, "count"))
        {
            if (!read_integer(document, object, "count", params.count, 1U,
                              std::numeric_limits<std::uint32_t>::max(),
                              "fade.count must be an integer in 1..4294967295", error))
            {
                return false;
            }
            params.has_count = true;
        }
    }
    return true;
}

bool parse_slip(const Document& document, const Value& object, SampleSlipParams& params, const char** error) noexcept
{
    const auto read_length = [&](const Value& value, std::uint32_t& out) noexcept {
        return read_integer(document, value, "length_samples", out, 1U, max_slip_length,
                            "sample_slip length_samples must be an integer in 1..1024", error);
    };
    const Value* events = document.find(object, "events");
    if (events != nullptr)
    {
        if (events->type != Type::array || events->child_count > max_events)
        {
            return set_error(error, "sample_slip.events must be an array of at most 32 objects");
        }
        for (std::uint32_t index = 0U; index < events->child_count; ++index)
        {
            const Value* item = document.at(*events, index);
            if (item == nullptr || item->type != Type::object)
            {
                return set_error(error, "sample_slip.events entries must be objects");
            }
            SlipEvent& event = params.events[index];
            if (!read_number(document, *item, "at_seconds", event.at_seconds, true, error) || !read_length(*item, event.length_samples))
            {
                return false;
            }
            const Value* kind = document.find(*item, "kind");
            if (kind == nullptr || !(kind->equals("delete") || kind->equals("duplicate")))
            {
                return set_error(error, "sample_slip kind must be delete or duplicate");
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
            return set_error(error, "sample_slip kind must be delete or duplicate");
        }
        std::uint32_t length = 0U;
        if (!read_length(object, length))
        {
            return false;
        }
        const Value* placement = document.find(object, "placement");
        double first = 0.0;
        double step = 0.0;
        std::uint32_t count = 1U;
        if (placement == nullptr || placement->equals("single"))
        {
            if (!read_number(document, object, "at_seconds", first, true, error))
            {
                return false;
            }
        }
        else if (placement->equals("spaced"))
        {
            if (!read_number(document, object, "first_seconds", first, true, error) || !read_number(document, object, "period_seconds", step, true, error) ||
                !read_integer(document, object, "count", count, 1U, max_events,
                              "sample_slip.count must be an integer in 1..32", error))
            {
                return false;
            }
        }
        else if (placement->equals("clustered"))
        {
            if (!read_number(document, object, "first_seconds", first, true, error) || !read_number(document, object, "spacing_seconds", step, true, error) ||
                !read_integer(document, object, "count", count, 1U, max_events,
                              "sample_slip.count must be an integer in 1..32", error))
            {
                return false;
            }
        }
        else
        {
            return set_error(error, "sample_slip.placement must be single, spaced or clustered");
        }
        params.event_count = count;
        for (std::uint32_t index = 0U; index < params.event_count; ++index)
        {
            params.events[index].at_seconds = first + static_cast<double>(index) * step;
            params.events[index].duplicate = kind->equals("duplicate");
            params.events[index].length_samples = length;
        }
    }
    return true;
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
    }
    return "?";
}

bool parse_scenario(const char* text, std::size_t length, Scenario& scenario, const char** error) noexcept
{
    scenario = Scenario{};
    Document& document = scenario_document;
    if (!document.parse(text, length))
    {
        return set_error(error, document.error());
    }
    const Value* root = document.root();
    if (root == nullptr || root->type != Type::object)
    {
        return set_error(error, "scenario must be a JSON object");
    }
    const Value* schema = document.find(*root, "schema");
    if (schema != nullptr && !schema->equals("signal-lab.scenario/1"))
    {
        return set_error(error, "unsupported scenario schema");
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
        if (seed->type != Type::number || seed->number < 0.0)
        {
            return set_error(error, "seed must be a non-negative integer");
        }
        scenario.seed = static_cast<std::uint64_t>(seed->number);
    }
    if (!read_number(document, *root, "source_gain_db", scenario.source_gain_db, false, error))
    {
        return false;
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
            return set_error(error, "clip_policy must be saturate or reject");
        }
    }
    const Value* reference = document.find(*root, "reference");
    if (reference != nullptr && !(reference->type == Type::string && reference->equals("auto")))
    {
        if (reference->type != Type::object)
        {
            return set_error(error, "reference must be \"auto\" or an interval object");
        }
        scenario.reference_auto = false;
        if (!read_number(document, *reference, "start_seconds", scenario.reference_start_seconds, true, error) ||
            !read_number(document, *reference, "duration_seconds", scenario.reference_duration_seconds, true, error))
        {
            return false;
        }
    }
    if (document.has(*root, "reference_rms"))
    {
        scenario.has_reference_rms = true;
        if (!read_number(document, *root, "reference_rms", scenario.reference_rms, true, error) || scenario.reference_rms <= 0.0)
        {
            return set_error(error, "reference_rms must be a positive number");
        }
    }
    const Value* impairments = document.find(*root, "impairments");
    if (impairments == nullptr)
    {
        return true;
    }
    if (impairments->type != Type::array)
    {
        return set_error(error, "impairments must be an array");
    }
    if (impairments->child_count > max_stages)
    {
        return set_error(error, "too many impairment stages (maximum 8)");
    }
    for (std::uint32_t index = 0U; index < impairments->child_count; ++index)
    {
        const Value* item = document.at(*impairments, index);
        if (item == nullptr || item->type != Type::object)
        {
            return set_error(error, "each impairment must be an object");
        }
        const Value* type = document.find(*item, "type");
        Stage& stage = scenario.stages[index];
        bool ok = false;
        if (type == nullptr)
        {
            return set_error(error, "impairment.type is required");
        }
        if (type->equals("awgn"))
        {
            stage.type = StageType::awgn;
            ok = parse_awgn(document, *item, stage.awgn, error);
        }
        else if (type->equals("cw"))
        {
            stage.type = StageType::cw;
            ok = parse_cw(document, *item, stage.cw, error);
        }
        else if (type->equals("impulse"))
        {
            stage.type = StageType::impulse;
            ok = parse_impulse(document, *item, stage.impulse, error);
        }
        else if (type->equals("fade"))
        {
            stage.type = StageType::fade;
            ok = parse_fade(document, *item, stage.fade, error);
        }
        else if (type->equals("sample_slip"))
        {
            stage.type = StageType::sample_slip;
            ok = parse_slip(document, *item, stage.slip, error);
        }
        else
        {
            return set_error(error, "unknown impairment type");
        }
        if (!ok)
        {
            return false;
        }
        scenario.stage_count = index + 1U;
    }
    for (std::uint32_t index = 0U; index + 1U < scenario.stage_count; ++index)
    {
        if (scenario.stages[index].type == StageType::sample_slip)
        {
            return set_error(error, "sample_slip must be the last impairment");
        }
    }
    return true;
}

} // namespace signal_lab
