// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// Host unit tests for the portable signal_lab engine (no framework; exit code = failures).

#include "signal_lab/det_math.hpp"
#include "signal_lab/json.hpp"
#include "signal_lab/mixer.hpp"
#include "signal_lab/sample_stream.hpp"
#include "signal_lab/scenario.hpp"

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace
{

int failures = 0;
int printed = 0;

void report(const char* what, const char* detail)
{
    ++failures;

    if (printed < 40)
    {
        ++printed;
        std::printf("FAIL: %s%s\n", what, detail);
    }
}

void check(bool condition, const char* what)
{
    if (!condition)
    {
        report(what, "");
    }
}

void check_near(double actual, double expected, double tolerance, const char* what)
{
    if (!(std::fabs(actual - expected) <= tolerance))
    {
        char detail[96];
        std::snprintf(detail, sizeof(detail), " (actual %.15g expected %.15g)", actual, expected);
        report(what, detail);
    }
}

signal_lab::Engine engine_a;
signal_lab::Engine engine_b;

// Whether `result` is a count of `frames`.
[[nodiscard]] bool produced(const m110::Result<std::size_t>& result, std::size_t frames)
{
    return result.has_value() && result.value() == frames;
}

// Whether `result` is the failure `failure`.
[[nodiscard]] bool failed_with(const m110::Result<std::size_t>& result, m110::Status failure)
{
    return !result.has_value() && signal_lab::same_failure(result.error(), failure);
}

constexpr std::size_t synthetic_signal_frames = std::size_t{3U} * signal_lab::sample_rate_hz;

// Deterministic pseudo-signal: three tones at -8.4 dBFS RMS, 3 seconds.
std::vector<std::int16_t> synthetic_signal(std::size_t frames = synthetic_signal_frames)
{
    std::vector<std::int16_t> pcm(frames);
    const double amplitude = 0.3803 * std::sqrt(2.0 / 3.0);

    for (std::size_t n = 0U; n < frames; ++n)
    {
        const double t = static_cast<double>(n) / 48000.0;
        const double x = amplitude * (signal_lab::det::sin(2.0 * signal_lab::det::pi * 700.0 * t) + signal_lab::det::sin(2.0 * signal_lab::det::pi * 1800.0 * t + 1.0) +
                                      signal_lab::det::sin(2.0 * signal_lab::det::pi * 2600.0 * t + 2.0));
        pcm[n] = static_cast<std::int16_t>(signal_lab::det::round_half_even(x * 32768.0));
    }

    return pcm;
}

double rms_of(const std::vector<std::int16_t>& pcm, std::size_t from = 0U, std::size_t to = 0U)
{
    if (to == 0U)
    {
        to = pcm.size();
    }

    double energy = 0.0;

    for (std::size_t n = from; n < to; ++n)
    {
        const double x = static_cast<double>(pcm[n]) / 32768.0;
        energy += x * x;
    }

    return std::sqrt(energy / static_cast<double>(to - from));
}

std::vector<std::int16_t> render(signal_lab::Engine& engine, const std::string& scenario_text, const std::vector<std::int16_t>& source, std::size_t block, double reference_rms)
{
    signal_lab::Scenario scenario;

    if (const m110::Status parsed = signal_lab::parse_scenario(scenario_text.c_str(), scenario_text.size(), scenario); !parsed.is_ok())
    {
        std::printf("scenario parse error: %s\n", parsed.message);
        ++failures;
        return {};
    }

    if (const m110::Status configured = engine.configure(scenario, reference_rms, source.size()); !configured.is_ok())
    {
        std::printf("configure error: %s\n", configured.message);
        ++failures;
        return {};
    }

    std::vector<std::int16_t> output;
    std::int16_t buffer[signal_lab::engine_capacity_frames];

    for (std::size_t cursor = 0U; cursor < source.size();)
    {
        const std::size_t frames = (source.size() - cursor) < block ? (source.size() - cursor) : block;
        const m110::Result<std::size_t> block = engine.process(source.data() + cursor, frames, buffer, signal_lab::engine_capacity_frames);

        if (!block.has_value())
        {
            std::printf("process error: %s\n", block.error().message);
            ++failures;
            return {};
        }

        output.insert(output.end(), buffer, buffer + block.value());
        cursor += frames;
    }

    return output;
}

void test_det_math()
{
    for (int i = -2000; i <= 2000; ++i)
    {
        const double x = static_cast<double>(i) * 0.0173;
        check_near(signal_lab::det::sin(x), std::sin(x), 2e-14, "det::sin");
        check_near(signal_lab::det::cos(x), std::cos(x), 2e-14, "det::cos");
    }

    for (int i = -300; i <= 300; ++i)
    {
        const double x = static_cast<double>(i) * 0.37;
        check_near(signal_lab::det::exp(x) / std::exp(x), 1.0, 1e-13, "det::exp");
        const double y = std::exp(x);
        check_near(signal_lab::det::log(y), x, 1e-12 * (1.0 + std::fabs(x)), "det::log");
    }

    check_near(signal_lab::det::sin(3.4e6), std::sin(3.4e6), 1e-9, "det::sin large argument");
    check(signal_lab::det::round_half_even(2.5) == 2.0, "round_half_even 2.5");
    check(signal_lab::det::round_half_even(3.5) == 4.0, "round_half_even 3.5");
    check(signal_lab::det::round_half_even(-2.5) == -2.0, "round_half_even -2.5");
    check(signal_lab::det::round_half_even(0.4999) == 0.0, "round_half_even 0.4999");
    check(signal_lab::det::seconds_to_frames(0.25, 48000U) == 12000U, "seconds_to_frames");
    // The edges (#10): what is not a positive duration is no frames, and a
    // count past the 64-bit counter saturates. Exactly 2^64 frames saturates
    // too, before a conversion that would be undefined, and so does a count
    // that is no number (+inf seconds at 0 Hz); the largest double below
    // 2^64, 2^64 - 2^11, still converts.
    constexpr std::uint32_t rate_hz = 48000U;
    constexpr std::uint32_t one_hz = 1U;
    constexpr std::uint32_t no_hz = 0U;
    constexpr double not_a_number = std::numeric_limits<double>::quiet_NaN();
    constexpr double infinity = std::numeric_limits<double>::infinity();
    constexpr double negative_seconds = -0.25;
    constexpr double frames_2_64 = 0x1p64;
    constexpr double largest_below_2_64 = 0x1.fffffffffffffp63;
    constexpr std::uint64_t largest_count_below_2_64 = 0xFFFFFFFFFFFFF800U;
    check(signal_lab::det::seconds_to_frames(not_a_number, rate_hz) == 0U, "seconds_to_frames NaN is 0");
    check(signal_lab::det::seconds_to_frames(negative_seconds, rate_hz) == 0U && signal_lab::det::seconds_to_frames(-infinity, rate_hz) == 0U, "seconds_to_frames of a negative duration is 0");
    check(signal_lab::det::seconds_to_frames(infinity, rate_hz) == UINT64_MAX, "seconds_to_frames +inf saturates");
    check(signal_lab::det::seconds_to_frames(infinity, no_hz) == UINT64_MAX, "seconds_to_frames of a NaN count saturates, never converting it");
    check(signal_lab::det::seconds_to_frames(frames_2_64, one_hz) == UINT64_MAX, "seconds_to_frames of exactly 2^64 frames saturates");
    check(signal_lab::det::seconds_to_frames(largest_below_2_64, one_hz) == largest_count_below_2_64, "seconds_to_frames of 2^64 - 2^11 frames converts");
    check_near(signal_lab::det::db_to_amplitude(-6.0), std::pow(10.0, -0.3), 1e-13, "db_to_amplitude");
    signal_lab::det::Pcg32 rng(42U, 1U, 0U);
    double sum = 0.0;
    double sum_sq = 0.0;

    for (int i = 0; i < 200000; ++i)
    {
        const double g = rng.gaussian();
        sum += g;
        sum_sq += g * g;
    }

    check_near(sum / 200000.0, 0.0, 0.01, "gaussian mean");
    check_near(sum_sq / 200000.0, 1.0, 0.01, "gaussian variance");
    signal_lab::det::Pcg32 again(42U, 1U, 0U);
    check(again.next() == signal_lab::det::Pcg32(42U, 1U, 0U).next(), "pcg32 reproducible");
    check(signal_lab::det::Pcg32(42U, 1U, 0U).next() != signal_lab::det::Pcg32(42U, 2U, 0U).next(), "pcg32 family separation");
}

void test_json()
{
    const char* literals[] = {"0", "7", "7.0", "-12", "0.25", "3.3e3", "1e-3", "123.456", "0.1", "1800", "0.38033893537026803"};
    signal_lab::json::Document document;

    for (const char* literal : literals)
    {
        check(document.parse(literal, std::strlen(literal)).is_ok(), "json number parses");
        const signal_lab::json::Value* root = document.root();
        check(root != nullptr && root->type == signal_lab::json::Type::number, "json number type");

        if (root != nullptr)
        {
            check(root->number == std::strtod(literal, nullptr), literal);
        }
    }

    const char* text = "{\"a\": [1, 2, {\"b\": \"x\\\"y\"}], \"c\": true, \"d\": null, \"e\": -3.5}";
    check(document.parse(text, std::strlen(text)).is_ok(), "json object parses");
    const signal_lab::json::Value* root = document.root();
    check(root != nullptr && root->child_count == 4U, "json object children");
    const signal_lab::json::Value* a = document.find(*root, "a");
    check(a != nullptr && a->type == signal_lab::json::Type::array && a->child_count == 3U, "json array");
    const signal_lab::json::Value* third = document.at(*a, 2U);
    check(third != nullptr && document.find(*third, "b") != nullptr, "json nested object");
    check(document.number_or(*root, "e", 0.0) == -3.5, "json number_or");
    const m110::Status bad_document = document.parse("{\"a\": }", 7U);
    check(bad_document.code == m110::StatusCode::invalid_argument && std::strcmp(bad_document.message, "unexpected character") == 0 && document.root() == nullptr,
          "json rejects bad document with invalid_argument and the reader's message");
}

// A number the reader reads, and the double it gives: bit for bit, as the
// reader computes it (the mantissa's first 19 significant digits times a
// power of ten, one rounding within 2^53 and 10^22; beyond that range the
// last bit may differ from strtod's, and a scenario's numbers must not).
struct JsonNumber
{
    const char* text;
    double value;
};

void test_json_numbers()
{
    constexpr auto numbers = std::to_array<JsonNumber>({
        // The 19th significant digit decides the rounding: 10^18 + 65 is above
        // the midpoint of two doubles, 10^18 + 60 below it.
        {"1000000000000000065", 0x1.bc16d674ec801p+59},
        {"0.1000000000000000065e19", 0x1.bc16d674ec801p+59},
        {"1000000000000000060", 0x1.bc16d674ec8p+59},
        {"12345678901234567890123", 0x1.4ea15b273b38ap+73},
        {"99999999999999999999999999", 0x1.4adf4b7320335p+86},
        {"0.12345678901234567890123", 0x1.f9add3746f65fp-4},
        {"123456789012345678.9", 0x1.b69b4ba630f35p+56},
        {"0.000000000000000000000000000001234", 0x1.9074b58c7cacap-100},
        {"18446744073709551615", 0x1p+64},
        {"9007199254740993", 0x1p+53},
        {"1e+5", 0x1.86ap+16},
        {"1E5", 0x1.86ap+16},
        {"1e00000000000000000000000000000001", 0x1.4p+3},
        {"1e22", 0x1.0f0cf064dd592p+73},
        {"1e-22", 0x1.e392010175ee6p-74},
        {"1e23", 0x1.52d02c7e14af6p+76},
        {"1e-23", 0x1.82db34012b252p-77},
        {"1e300", 0x1.7e43c8800759dp+996},
        {"1e-300", 0x1.56e1fc2f8f359p-997},
        {"1e-310", 0x0.012688b70e62bp-1022},
        {"4.9e-324", 0x0.0000000000001p-1022},
        {"6.02214076e23", 0x1.fe185ca57c517p+78},
        {"1234.5678e-2", 0x1.8b0fcb4f1e4b4p+3},
        {"-0.0", -0x0p+0},
        {"-0", -0x0p+0},
        // An exponent stops growing once it reaches 10^4, so one beyond int's
        // range (2^32 + 1 here) still underflows to zero, and its positive
        // twin is refused (test_json_refusals).
        {"1e-4294967297", 0x0p+0},
    });
    signal_lab::json::Document document;

    for (const JsonNumber& number : numbers)
    {
        const bool read = document.parse(number.text, std::strlen(number.text)).is_ok() && document.root() != nullptr && document.root()->type == signal_lab::json::Type::number &&
                          std::bit_cast<std::uint64_t>(document.root()->number) == std::bit_cast<std::uint64_t>(number.value);
        check(read, number.text);
    }
}

// A document the reader refuses, and the message it gives: each of its
// refusals, the first one it meets.
struct JsonRefusal
{
    std::string text;
    const char* message;
};

// The reader's limits: the nesting it reads, and the characters of a refused
// document a failure shows.
constexpr std::size_t json_deepest = 32U;
constexpr std::size_t json_shown_characters = 40U;

// An array of `count` zeros, inside `depth` arrays.
[[nodiscard]] std::string nested_zeros(std::size_t depth, std::size_t count)
{
    std::string text(depth, '[');

    for (std::size_t index = 0U; index < count; ++index)
    {
        text += index == 0U ? "0" : ",0";
    }

    text.append(depth, ']');
    return text;
}

// `depth` objects, each the one member of the one around it.
[[nodiscard]] std::string nested_objects(std::size_t depth)
{
    std::string text;

    for (std::size_t level = 1U; level < depth; ++level)
    {
        text += R"({"a": )";
    }

    text += "{}";
    text.append(depth - 1U, '}');
    return text;
}

// A member "a" holding `inner`.
[[nodiscard]] std::string member_of(const std::string& inner)
{
    std::string text = R"({"a": )";
    text += inner;
    text += '}';
    return text;
}

void test_json_refusals()
{
    const std::vector<JsonRefusal> refusals{
        {"", "unexpected end of document"},
        {" \t\r\n", "unexpected end of document"},
        {"x", "unexpected character"},
        {"tru", "unexpected character"},
        {"1 2", "trailing characters after the document"},
        {"{1: 2}", "expected a string"},
        {"{", "expected a string"},
        {R"("abc)", "unterminated string"},
        {R"("abc\)", "unterminated string"},
        {"-", "expected a digit"},
        {"-x", "expected a digit"},
        {"1.", "expected a fraction digit"},
        {"1.e5", "expected a fraction digit"},
        {"1e", "expected an exponent digit"},
        {"1e+", "expected an exponent digit"},
        {"1e400", "JSON number is outside the finite range"},
        {"-1e400", "JSON number is outside the finite range"},
        {"1e4294967297", "JSON number is outside the finite range"},
        {R"({"a" 1})", "expected ':' after an object key"},
        {R"({"a": 1)", "unterminated object"},
        {R"({"a": 1 "b": 2})", "expected ',' or '}' in an object"},
        {R"({"a": })", "unexpected character"},
        {"[1", "unterminated array"},
        {"[1 2]", "expected ',' or ']' in an array"},
        {"[1,]", "unexpected character"},
        // A container closed by the other bracket, after a member, empty, or
        // inside another.
        {R"({"a": 1])", "expected ',' or '}' in an object"},
        {"[1}", "expected ',' or ']' in an array"},
        {"{]", "expected a string"},
        {"[}", "unexpected character"},
        {R"([{"a": [1}]])", "expected ',' or ']' in an array"},
        {R"([{"a": [1, -]}])", "expected a digit"},
        // 32 levels of nesting are read, the 33rd is refused.
        {nested_zeros(json_deepest + 1U, 1U), "document nests too deeply"},
        {nested_objects(json_deepest + 1U), "document nests too deeply"},
        {member_of(nested_zeros(json_deepest, 1U)), "document nests too deeply"},
        // The root and 255 elements fill the 256 values; one more does not fit.
        {nested_zeros(1U, signal_lab::json::document_capacity), "document has too many values"},
    };
    signal_lab::json::Document document;

    for (const JsonRefusal& refusal : refusals)
    {
        const m110::Status parsed = document.parse(refusal.text.data(), refusal.text.size());
        const bool refused = parsed.code == m110::StatusCode::invalid_argument && std::strcmp(parsed.message, refusal.message) == 0 && document.root() == nullptr;

        if (!refused)
        {
            std::string detail = refusal.text.substr(0U, json_shown_characters);
            detail += ": expected \"";
            detail += refusal.message;
            detail += "\", got \"";
            detail += parsed.message;
            detail += '"';
            report("json refusal of ", detail.c_str());
        }
    }

    const m110::Status null_text = document.parse(nullptr, 0U);
    check(null_text.code == m110::StatusCode::invalid_argument && std::strcmp(null_text.message, "null text") == 0 && document.root() == nullptr, "json refuses a null text");
    // The limits themselves are read: 32 levels (the zero inside them is the
    // 33rd value), 256 values.
    const std::string deepest = nested_zeros(json_deepest, 1U);
    check(document.parse(deepest.data(), deepest.size()).is_ok() && document.node_count() == json_deepest + 1U && document.root() != nullptr, "json reads 32 levels of nesting");
    const std::string deepest_objects = nested_objects(json_deepest);
    check(document.parse(deepest_objects.data(), deepest_objects.size()).is_ok() && document.node_count() == json_deepest, "json reads 32 levels of nested objects");
    const std::string fullest = nested_zeros(1U, signal_lab::json::document_capacity - 1U);
    check(document.parse(fullest.data(), fullest.size()).is_ok() && document.node_count() == signal_lab::json::document_capacity && document.root() != nullptr &&
              document.root()->child_count == signal_lab::json::document_capacity - 1U,
          "json reads a document of 256 values");
    // A refusal leaves nothing behind for the next document: an object of one
    // member, an array of five values.
    const std::string bad = "[1 2]";
    const std::string good = R"({"k": [true, false, null, "s", -0.5]})";
    constexpr std::uint32_t good_values = 7U;
    check(!document.parse(bad.data(), bad.size()).is_ok() && document.parse(good.data(), good.size()).is_ok() && document.root() != nullptr && document.root()->child_count == 1U &&
              document.node_count() == good_values,
          "json reads a document after refusing one");
}

// One value of the node table: its type, its key (nullptr outside an object),
// its first child, its next sibling (json_none: none) and its child count.
struct JsonNode
{
    signal_lab::json::Type type;
    const char* key;
    std::uint32_t first_child;
    std::uint32_t next_sibling;
    std::uint32_t child_count;
};

constexpr std::uint32_t json_none = 0xFFFFFFFFU;

// The node table, link by link: values in document order (a container before
// its members), each member linked to the next, keys only in objects, empty
// containers and a container's last member with no link. find(), at() and
// the scenario reader walk these links.
void test_json_structure()
{
    using signal_lab::json::Type;
    const std::string text = R"({"a": [1, {"b": "x"}, []], "c": {}, "d": [[true], null], "e": -2.5})";
    constexpr auto nodes = std::to_array<JsonNode>({
        {Type::object, nullptr, 1U, json_none, 4U},
        {Type::array, "a", 2U, 6U, 3U},
        {Type::number, nullptr, json_none, 3U, 0U},
        {Type::object, nullptr, 4U, 5U, 1U},
        {Type::string, "b", json_none, json_none, 0U},
        {Type::array, nullptr, json_none, json_none, 0U},
        {Type::object, "c", json_none, 7U, 0U},
        {Type::array, "d", 8U, 11U, 2U},
        {Type::array, nullptr, 9U, 10U, 1U},
        {Type::boolean, nullptr, json_none, json_none, 0U},
        {Type::null, nullptr, json_none, json_none, 0U},
        {Type::number, "e", json_none, json_none, 0U},
    });
    signal_lab::json::Document document;
    check(document.parse(text.data(), text.size()).is_ok() && document.node_count() == nodes.size(), "json structure: every value is a node");

    for (std::uint32_t index = 0U; index < nodes.size() && index < document.node_count(); ++index)
    {
        const JsonNode& expected = nodes.at(index);
        const signal_lab::json::Value& node = *document.child(index);
        const bool key_right = expected.key == nullptr ? (node.key == nullptr && node.key_length == 0U) : std::string_view{node.key, node.key_length} == expected.key;
        const bool right =
            node.type == expected.type && key_right && node.first_child == expected.first_child && node.next_sibling == expected.next_sibling && node.child_count == expected.child_count;

        if (!right)
        {
            const std::string detail = std::to_string(index);
            report("json structure: node ", detail.c_str());
        }
    }

    // The scalars keep their values: a's 1, b's "x", d's true and e's -2.5.
    constexpr std::uint32_t one_node = 2U;
    constexpr std::uint32_t x_node = 4U;
    constexpr std::uint32_t true_node = 9U;
    constexpr std::uint32_t e_node = 11U;
    constexpr double e_value = -2.5;
    check(document.node_count() == nodes.size() && document.child(one_node)->number == 1.0 && document.child(x_node)->equals("x") && document.child(true_node)->boolean &&
              document.child(e_node)->number == e_value,
          "json structure: scalar values");
}

const std::string mix005 = "{\"schema\": \"signal-lab.scenario/1\", \"test_id\": \"m110_600L_mix_MIX005_seed0234\", \"seed\": 234,"
                           " \"source\": {\"path\": \"../reference/x.wav\"}, \"reference\": \"auto\", \"source_gain_db\": -12.0, \"clip_policy\": \"saturate\","
                           " \"impairments\": ["
                           "  {\"type\": \"fade\", \"depth_db\": 24, \"duration_ms\": 250, \"shape\": \"raised_cosine\", \"first_seconds\": 1, \"period_seconds\": 1},"
                           "  {\"type\": \"awgn\", \"snr_db\": 3.0},"
                           "  {\"type\": \"cw\", \"frequency_hz\": 1800, \"ci_db\": 3},"
                           "  {\"type\": \"impulse\", \"rate_per_sec\": 1.0, \"peak_db\": 20, \"decay_ms\": 20},"
                           "  {\"type\": \"sample_slip\", \"kind\": \"delete\", \"length_samples\": 5, \"placement\": \"spaced\", \"first_seconds\": 0.5, \"period_seconds\": 1, \"count\": 2}"
                           " ]}";

void test_scenario()
{
    signal_lab::Scenario scenario;
    check(signal_lab::parse_scenario(mix005.c_str(), mix005.size(), scenario).is_ok(), "MIX-005 parses");
    check(scenario.stage_count == 5U, "five stages");
    check(scenario.seed == 234U && scenario.source_gain_db == -12.0 && scenario.saturate, "scenario scalars");
    check(std::strcmp(scenario.test_id, "m110_600L_mix_MIX005_seed0234") == 0, "test_id");
    check(scenario.stages[0].type == signal_lab::StageType::fade && scenario.stages[0].fade.depth_db == 24.0, "fade stage");
    constexpr double mix005_snr_db = 3.0;
    check(scenario.stages[1].type == signal_lab::StageType::awgn && scenario.stages[1].awgn.snr.in(signal_lab::units::dB) == mix005_snr_db, "awgn stage");
    check(scenario.stages[4].type == signal_lab::StageType::sample_slip && scenario.stages[4].slip.event_count == 2U && scenario.stages[4].slip.events[1].at_seconds == 1.5, "slip placement expands");
    const std::string bad =
        "{\"impairments\": [{\"type\": \"sample_slip\", \"kind\": \"delete\", \"length_samples\": 1, \"at_seconds\": 1}, {\"type\": \"cw\", \"frequency_hz\": 1800, \"ci_db\": 3}]}";
    const m110::Status slip_not_last = signal_lab::parse_scenario(bad.c_str(), bad.size(), scenario);
    check(slip_not_last.code == m110::StatusCode::invalid_argument && std::strcmp(slip_not_last.message, "sample_slip must be the last impairment") == 0, "slip must be last");
    const std::string with_reference = "{\"reference_rms\": 0.25, \"impairments\": []}";
    check(signal_lab::parse_scenario(with_reference.c_str(), with_reference.size(), scenario).is_ok() && scenario.has_reference_rms && scenario.reference_rms == 0.25, "reference_rms");

    for (const char* count : {"0", "-1", "1.5", "true", "4294967296", "1e300", "1e999"})
    {
        const std::string fade = std::string("{\"impairments\":[{\"type\":\"fade\",\"depth_db\":18,\"duration_ms\":250,\"period_seconds\":1,\"count\":") + count + "}]}";
        check(!signal_lab::parse_scenario(fade.c_str(), fade.size(), scenario).is_ok(), "fade rejects invalid count before conversion");
    }
}

void test_engine_determinism()
{
    const auto source = synthetic_signal();
    const double reference = rms_of(source);
    const auto a = render(engine_a, mix005, source, 2048U, reference);
    const auto b = render(engine_b, mix005, source, 2048U, reference);
    check(!a.empty() && a == b, "same scenario renders identically");
    check(engine_a.stats().output_digest == engine_b.stats().output_digest, "digests match");
    const auto c = render(engine_b, mix005, source, 1000U, reference);
    check(a == c, "block size does not change the output");
    check(a.size() == source.size() - 10U, "two 5-sample deletions shorten the stream");
    std::string other = mix005;
    other.replace(other.find("\"seed\": 234"), 11U, "\"seed\": 235");
    const auto d = render(engine_b, other, source, 2048U, reference);
    check(d != a, "seed changes the output");
    check(engine_a.stats().arena_bytes_used > 0U && engine_a.stats().stage_count == 5U, "arena in use");
}

void test_levels()
{
    const auto source = synthetic_signal();
    const double reference = rms_of(source);
    const std::string cw = "{\"source_gain_db\": 0, \"impairments\": [{\"type\": \"cw\", \"frequency_hz\": 1800, \"ci_db\": 6, \"ramp_seconds\": 0}]}";
    const auto with_cw = render(engine_a, cw, source, 2048U, reference);
    double energy = 0.0;

    for (std::size_t n = 0U; n < source.size(); ++n)
    {
        const double d = static_cast<double>(with_cw[n] - source[n]) / 32768.0;
        energy += d * d;
    }

    const double tone_rms = std::sqrt(energy / static_cast<double>(source.size()));
    check_near(20.0 * std::log10(reference / tone_rms), 6.0, 0.05, "cw C/I");
    const std::string awgn = "{\"source_gain_db\": 0, \"impairments\": [{\"type\": \"awgn\", \"snr_db\": 10}]}";
    const auto with_noise = render(engine_a, awgn, source, 2048U, reference);
    energy = 0.0;

    for (std::size_t n = 0U; n < source.size(); ++n)
    {
        const double d = static_cast<double>(with_noise[n] - source[n]) / 32768.0;
        energy += d * d;
    }

    // The label is N0 x 3000 Hz (awgn.hpp); the total noise is N0 x ENBW, so the
    // total-power SNR measured here is lower by 10 log10(ENBW / 3000 Hz), 0.325 dB.
    constexpr double awgn_snr_tolerance_db = 0.3;
    const double total_power_offset = 10.0 * std::log10(signal_lab::noise_band_kernel().enbw / signal_lab::snr_reference_bandwidth);
    const double measured_snr_db = 20.0 * std::log10(reference / std::sqrt(energy / static_cast<double>(source.size())));
    const double expected_snr_db = 10.0 - total_power_offset;
    check_near(measured_snr_db, expected_snr_db, awgn_snr_tolerance_db, "awgn SNR");
    const std::string fade = "{\"source_gain_db\": 0, \"impairments\": [{\"type\": \"fade\", \"depth_db\": 18, \"duration_ms\": 250, \"starts_seconds\": [1.0]}]}";
    const auto faded = render(engine_a, fade, source, 2048U, reference);
    bool untouched = true;

    for (std::size_t n = 0U; n < 48000U; ++n)
    {
        untouched = untouched && faded[n] == source[n];
    }

    for (std::size_t n = 48000U + 12000U + 1U; n < source.size(); ++n)
    {
        untouched = untouched && faded[n] == source[n];
    }

    check(untouched, "fade leaves other regions untouched");
    const double mid = rms_of(faded, 48000U + 3000U, 48000U + 9000U) / rms_of(source, 48000U + 3000U, 48000U + 9000U);
    check_near(20.0 * std::log10(mid), -18.0, 0.1, "fade hold depth");
    check(engine_a.stage(0)->stats().events_applied == 1U, "fade event counted");
    // +12 dB above a -12 dB scaled reference stays inside full scale, so the first sample equals A exactly.
    const std::string passthrough = "{\"source_gain_db\": -12, \"impairments\": []}";
    const auto scaled = render(engine_b, passthrough, source, 2048U, reference);
    const std::string impulse = "{\"source_gain_db\": -12, \"impairments\": [{\"type\": \"impulse\", \"period_seconds\": 1.0, \"first_seconds\": 0.5, \"peak_db\": 12, \"decay_ms\": 5, \"ring_hz\": "
                                "0, \"phase_degrees\": 0}]}";
    const auto crashed = render(engine_a, impulse, source, 2048U, reference);
    const double first = static_cast<double>(crashed[24000] - scaled[24000]) / 32768.0;
    check_near(first, engine_a.scaled_reference_rms() * signal_lab::det::db_to_amplitude(12.0), 2.0 / 32768.0, "impulse peak amplitude");
    check(engine_a.stage(0)->stats().events_applied == 3U, "three periodic crashes");
    const std::string ring = "{\"source_gain_db\": -12, \"impairments\": [{\"type\": \"impulse\", \"rate_per_sec\": 5, \"peak_db\": 30, \"decay_ms\": 35, \"ring\": \"noise\"}]}";
    const auto noisy = render(engine_a, ring, source, 2048U, reference);
    check(!noisy.empty() && engine_a.stage(0)->stats().events_applied > 5U, "noise-ringing crashes scheduled");
}

void test_slips()
{
    std::vector<std::int16_t> ramp(20000U);

    for (std::size_t n = 0U; n < ramp.size(); ++n)
    {
        ramp[n] = static_cast<std::int16_t>(n % 30000U);
    }

    const std::string del = "{\"source_gain_db\": 0, \"impairments\": [{\"type\": \"sample_slip\", \"events\": [{\"at_seconds\": 0.1, \"kind\": \"delete\", \"length_samples\": 3}]}]}";
    auto out = render(engine_a, del, ramp, 2048U, 0.3);
    check(out.size() == ramp.size() - 3U && out[4799] == 4799 && out[4800] == 4803, "delete removes [4800, 4803)");
    const std::string dup = "{\"source_gain_db\": 0, \"impairments\": [{\"type\": \"sample_slip\", \"events\": [{\"at_seconds\": 0.1, \"kind\": \"duplicate\", \"length_samples\": 3}]}]}";
    out = render(engine_a, dup, ramp, 2048U, 0.3);
    check(out.size() == ramp.size() + 3U && out[4800] == 4797 && out[4802] == 4799 && out[4803] == 4800, "duplicate re-emits the last three");
    // Boundary-straddling delete with a tiny block size (delete carries across blocks).
    const std::string del48 = "{\"source_gain_db\": 0, \"impairments\": [{\"type\": \"sample_slip\", \"events\": [{\"at_seconds\": 0.0999, \"kind\": \"delete\", \"length_samples\": 48}]}]}";
    out = render(engine_a, del48, ramp, 100U, 0.3);
    const std::size_t at = static_cast<std::size_t>(signal_lab::det::seconds_to_frames(0.0999, 48000U));
    check(out.size() == ramp.size() - 48U && out[at] == static_cast<std::int16_t>(at + 48U), "48-sample delete across blocks");
    const auto reference_out = render(engine_b, del48, ramp, 2048U, 0.3);
    check(out == reference_out, "slip output independent of block size");
}

void test_slip_validation_and_capacity()
{
    signal_lab::Scenario scenario;

    for (const char* length : {"0", "-1", "1.5", "1024.5", "1025", "48000", "4294967297", "1e300", "1e999"})
    {
        const std::string compact = std::string("{\"impairments\":[{\"type\":\"sample_slip\",\"kind\":\"delete\",\"at_seconds\":0,\"length_samples\":") + length + "}]}";
        check(!signal_lab::parse_scenario(compact.c_str(), compact.size(), scenario).is_ok(), "compact slip rejects invalid length before conversion");
        const std::string explicit_event = std::string("{\"impairments\":[{\"type\":\"sample_slip\",\"events\":[{\"kind\":\"delete\",\"at_seconds\":0,\"length_samples\":") + length + "}]}]}";
        check(!signal_lab::parse_scenario(explicit_event.c_str(), explicit_event.size(), scenario).is_ok(), "explicit slip rejects invalid length before conversion");
    }

    for (const char* count : {"0", "-1", "1.5", "true", "32.5", "33", "4294967297", "1e300", "1e999"})
    {
        const std::string slip =
            std::string("{\"impairments\":[{\"type\":\"sample_slip\",\"kind\":\"delete\",\"placement\":\"spaced\",\"first_seconds\":0,\"period_seconds\":0.001,\"length_samples\":1,\"count\":") +
            count + "}]}";
        check(!signal_lab::parse_scenario(slip.c_str(), slip.size(), scenario).is_ok(), "sample slip rejects invalid count before conversion");
    }

    std::vector<std::int16_t> source(8192U);

    for (std::size_t index = 0U; index < source.size(); ++index)
    {
        source[index] = static_cast<std::int16_t>(10000U + index);
    }

    const auto configure = [&](const std::string& text)
    {
        check(signal_lab::parse_scenario(text.c_str(), text.size(), scenario).is_ok(), "slip regression scenario parses");
        return engine_a.configure(scenario, 0.3, source.size());
    };

    const std::string no_history =
        R"({"source_gain_db":0,"impairments":[{"type":"sample_slip","events":[{"kind":"delete","at_seconds":0,"length_samples":499},{"kind":"duplicate","at_seconds":0.010416666666666666,"length_samples":400}]}]})";
    const m110::Status deleted_history = configure(no_history);
    check(deleted_history.code == m110::StatusCode::invalid_configuration && std::strstr(deleted_history.message, "previously emitted") != nullptr, "deletion cannot supply duplicate history");
    const std::string too_large = R"({"source_gain_db":0,"impairments":[{"type":"sample_slip","kind":"duplicate","at_seconds":0.01,"length_samples":400}]})";
    check(std::strstr(configure(too_large).message, "256") != nullptr, "single duplicate exceeding output slack is rejected before rendering");
    const std::string too_dense =
        R"({"source_gain_db":0,"impairments":[{"type":"sample_slip","kind":"duplicate","placement":"clustered","first_seconds":0.01,"spacing_seconds":0.001,"count":3,"length_samples":128}]})";
    check(std::strstr(configure(too_dense).message, "256") != nullptr, "clustered duplicate expansion is checked as a whole");
    const std::string maximum_delete = R"({"source_gain_db":0,"impairments":[{"type":"sample_slip","kind":"delete","at_seconds":0.01,"length_samples":1024}]})";
    const auto deleted = render(engine_a, maximum_delete, source, 2048U, 0.3);
    check(deleted.size() == source.size() - signal_lab::max_slip_length && deleted[480] == source[1504], "1024-sample deletion remains supported");
    check(deleted == render(engine_b, maximum_delete, source, 100U, 0.3), "maximum deletion is independent of block size");
    const std::string within_slack =
        R"({"source_gain_db":0,"impairments":[{"type":"sample_slip","kind":"duplicate","placement":"clustered","first_seconds":0.01,"spacing_seconds":0.001,"count":2,"length_samples":128}]})";
    const auto expanded = render(engine_a, within_slack, source, 2048U, 0.3);
    check(expanded.size() == source.size() + signal_lab::engine_slack_frames && expanded.back() == source.back(), "all source frames survive maximum clustered expansion");
    check(expanded == render(engine_b, within_slack, source, 100U, 0.3), "maximum clustered expansion is independent of block size");
    const std::string separated =
        R"({"source_gain_db":0,"impairments":[{"type":"sample_slip","kind":"duplicate","placement":"spaced","first_seconds":0.01,"period_seconds":0.042666666666666665,"count":2,"length_samples":256}]})";
    const auto separate_output = render(engine_a, separated, source, 2048U, 0.3);
    check(separate_output.size() == source.size() + 512U && separate_output == render(engine_b, separated, source, 100U, 0.3), "duplicates exactly one maximum block apart are supported");
    const std::string dropped_duplicate =
        R"({"source_gain_db":0,"impairments":[{"type":"sample_slip","events":[{"kind":"delete","at_seconds":0,"length_samples":1024},{"kind":"duplicate","at_seconds":0.01,"length_samples":400}]}]})";
    const auto dropped = render(engine_a, dropped_duplicate, source, 2048U, 0.3);
    check(dropped.size() == source.size() - 1024U && engine_a.stats().events_dropped == 1U, "duplicate inside deleted input does not consume expansion or history");
    check(dropped == render(engine_b, dropped_duplicate, source, 100U, 0.3), "dropped duplicate is independent of block size");
    // Exercise stage defenses when the caller violates continuity or provides too
    // little expansion space: source samples must still survive without bad reads.
    alignas(signal_lab::impairment_alignment) unsigned char storage[8192]{};
    float scratch[signal_lab::engine_capacity_frames]{};
    float samples[signal_lab::engine_capacity_frames]{};

    for (std::size_t index = 0U; index < 480U; ++index)
    {
        samples[index] = static_cast<float>(index);
    }

    signal_lab::SampleSlipParams params;
    params.event_count = 1U;
    params.events[0] = {0.01, true, 128U};
    const signal_lab::PlacedStage placed = signal_lab::construct_sample_slip(params, storage, sizeof(storage));
    check(placed.has_value(), "standalone slip stage is placed");

    if (!placed.has_value())
    {
        return;
    }

    signal_lab::Impairment& stage = placed.value();
    check(stage.prepare({0.3, 8192U, 0U, 0U, scratch}).is_ok(), "standalone slip stage prepares");
    check(produced(stage.process(samples, 200U, 480U, signal_lab::engine_capacity_frames), 200U) && stage.stats().events_dropped == 1U && samples[199] == 199.0f,
          "missing runtime history drops the duplicate even with sufficient output space");
    check(stage.prepare({0.3, 8192U, 0U, 0U, scratch}).is_ok(), "standalone slip stage resets history");
    check(produced(stage.process(samples, 480U, 0U, signal_lab::engine_capacity_frames), 480U), "standalone slip stage establishes enough duplicate history");
    check(produced(stage.process(samples, 200U, 480U, 200U), 200U) && stage.stats().events_dropped == 1U && samples[199] == 199.0f,
          "insufficient expansion space preserves source frames even with valid history");
    check(signal_lab::same_failure(signal_lab::construct_sample_slip(params, storage, signal_lab::sample_slip_size() - 1U).error(), signal_lab::placement_refused),
          "storage smaller than the stage is placement_refused");
    signal_lab::Stage awgn;
    awgn.type = signal_lab::StageType::awgn;
    check(signal_lab::same_failure(signal_lab::construct_impairment(awgn, storage, sizeof(storage)).error(), signal_lab::not_an_arena_stage), "a typed-slot stage is not built in the arena");
}

void test_fade_event_boundaries()
{
    const std::vector<std::int16_t> source(8192U, 10000);
    const std::string spaced = R"({"source_gain_db":0,"impairments":[{"type":"fade","depth_db":20,"duration_ms":1,"shape":"rectangular","period_seconds":0.002,"count":32}]})";
    const auto output = render(engine_a, spaced, source, 2048U, 0.3);
    check(engine_a.stats().events_applied == 32U && engine_a.stats().events_dropped == 0U, "nonoverlapping fades do not compete for active slots");
    // The scenario's 32 fades start every 2 ms (96 frames) and last 1 ms (48 frames).
    constexpr std::size_t fade_count = 32U;
    constexpr std::size_t fade_period_frames = 96U;
    constexpr std::size_t fade_frames = 48U;

    for (std::size_t index = 0U; index < source.size(); ++index)
    {
        const bool faded = index < fade_count * fade_period_frames && index % fade_period_frames < fade_frames;
        check(output[index] == (faded ? 1000 : 10000), "every scheduled rectangular fade has the expected samples");
    }

    check(output == render(engine_b, spaced, source, 100U, 0.3), "nonoverlapping fades are independent of block size");
    // Eight fades end exactly when the next eight start; the ninth simultaneous
    // fade in each group is the only event dropped at either block size.
    const std::string overlap =
        R"({"source_gain_db":0,"impairments":[{"type":"fade","depth_db":1,"duration_ms":1,"shape":"rectangular","starts_seconds":[0,0,0,0,0,0,0,0,0,0.001,0.001,0.001,0.001,0.001,0.001,0.001,0.001,0.001]}]})";
    const auto overlapping = render(engine_a, overlap, source, 2048U, 0.3);
    check(engine_a.stats().events_applied == 16U && engine_a.stats().events_dropped == 2U, "fade capacity counts true simultaneous events and reuses finished slots");
    check(overlapping == render(engine_b, overlap, source, 17U, 0.3) && engine_b.stats().events_applied == 16U && engine_b.stats().events_dropped == 2U,
          "overlap admission and retirement are independent of block size");
}

void test_processing_failures_and_emitted_levels()
{
    signal_lab::Scenario scenario;
    scenario.source_gain_db = 6.0;
    scenario.saturate = true;
    const std::int16_t input[] = {0, 1, -1, 123, -123, 32767, -32768};
    std::int16_t output[signal_lab::engine_capacity_frames]{};
    check(engine_a.configure(scenario, 0.3, 7U).is_ok(), "saturating level scenario configures");
    check(produced(engine_a.process(input, 7U, output, signal_lab::engine_capacity_frames), 7U) && engine_a.status().is_ok(), "saturation still emits every frame");
    double emitted_energy = 0.0;

    for (std::size_t index = 0U; index < 7U; ++index)
    {
        const double sample = static_cast<double>(output[index]) / 32768.0;
        emitted_energy += sample * sample;
    }

    check(output[5] == 32767 && output[6] == -32768 && engine_a.stats().clipped_samples == 2U, "saturation clamps both PCM rails");
    check(engine_a.stats().peak == 1.0f && engine_a.stats().output_energy == emitted_energy, "level statistics measure quantized emitted PCM after saturation");
    scenario.saturate = false;
    const std::int16_t good[] = {1, -1, 123, -123};
    const std::int16_t bad[] = {32767, 10, -32768, 20};
    check(engine_a.configure(scenario, 0.3, 8U).is_ok(), "rejecting level scenario configures");
    check(produced(engine_a.process(good, 4U, output, signal_lab::engine_capacity_frames), 4U), "unclipped prefix is accepted");
    const signal_lab::EngineStats prefix = engine_a.stats();
    check(failed_with(engine_a.process(bad, 4U, output, signal_lab::engine_capacity_frames), signal_lab::engine_failure::clipping) &&
              signal_lab::same_failure(engine_a.status(), signal_lab::engine_failure::clipping),
          "clip policy rejects the entire offending block, and the engine latches the clipping Status");
    const signal_lab::EngineStats rejected = engine_a.stats();
    signal_lab::det::StreamDigest consumed;
    consumed.update(good, 4U);
    consumed.update(bad, 4U);
    check(rejected.frames_in == 8U && rejected.source_digest == consumed.value(), "rejected block is counted as consumed source");
    check(rejected.frames_out == prefix.frames_out && rejected.output_digest == prefix.output_digest && rejected.peak == prefix.peak && rejected.output_energy == prefix.output_energy,
          "rejected block commits no output frames digest or levels");
    check(rejected.clipped_samples == 2U && rejected.first_clipped_frame == 4U, "clipping diagnostic identifies first rejected output frame");
    check(failed_with(engine_a.process(good, 4U, output, signal_lab::engine_capacity_frames), signal_lab::engine_failure::clipping) && engine_a.stats().frames_in == rejected.frames_in,
          "processing failure remains latched without consuming further input");
    check(engine_a.configure(scenario, 0.3, 4U).is_ok() && engine_a.status().is_ok() && produced(engine_a.process(good, 4U, output, signal_lab::engine_capacity_frames), 4U),
          "configure clears a previous processing failure");
    engine_a.configure_passthrough();
    check(failed_with(engine_a.process(good, 4U, output, 4U + signal_lab::engine_slack_frames - 1U), signal_lab::engine_failure::invalid_block) &&
              signal_lab::same_failure(engine_a.status(), signal_lab::engine_failure::invalid_block) && engine_a.stats().frames_in == 0U,
          "insufficient output slack fails before consuming input");
    engine_a.configure_passthrough();
    check(engine_a.status().is_ok() && produced(engine_a.process(good, 4U, output, signal_lab::engine_capacity_frames), 4U) && output[0] == good[0] && output[3] == good[3],
          "passthrough configuration clears failures and preserves samples");
    signal_lab::Engine unconfigured;
    check(failed_with(unconfigured.process(good, 4U, output, signal_lab::engine_capacity_frames), signal_lab::engine_failure::not_configured) &&
              unconfigured.status().code == m110::StatusCode::unavailable && unconfigured.stats().frames_in == 0U,
          "an unconfigured engine refuses a block and latches not_configured");
}

void test_pipeline_error_propagation()
{
    class Source final : public signal_lab::SampleSource
    {
    public:
        const std::int16_t* samples{};
        std::size_t length{};
        std::size_t cursor{};
        // A read that starts at or after this frame fails with `failure`.
        std::size_t failing_at{std::numeric_limits<std::size_t>::max()};
        m110::Status failure{};
        [[nodiscard]] m110::Result<std::size_t> read(std::int16_t* frames, std::size_t capacity) noexcept override
        {
            if (cursor >= failing_at)
            {
                return failure;
            }

            std::size_t count = length - cursor;
            count = count < 4U ? count : 4U;
            count = count < capacity ? count : capacity;

            for (std::size_t index = 0U; index < count; ++index)
            {
                frames[index] = samples[cursor++];
            }

            return count;
        }

        [[nodiscard]] std::uint64_t total_frames() const noexcept override { return length; }
    } source;

    class Sink final : public signal_lab::SampleSink
    {
    public:
        std::size_t frames{};
        std::size_t calls{};
        [[nodiscard]] m110::Status write(const std::int16_t*, std::size_t count) noexcept override
        {
            frames += count;
            ++calls;
            return m110::Status::success();
        }
    } sink;

    const std::int16_t samples[] = {1, -1, 123, -123, 32767, 10, -32768, 20};
    source.samples = samples;
    source.length = 8U;
    signal_lab::Scenario scenario;
    scenario.source_gain_db = 6.0;
    scenario.saturate = false;
    std::int16_t input_block[signal_lab::engine_block_frames]{};
    std::int16_t output_block[signal_lab::engine_capacity_frames]{};
    check(engine_a.configure(scenario, 0.3, source.total_frames()).is_ok(), "pipeline rejecting scenario configures");
    check(signal_lab::same_failure(signal_lab::run_pipeline(source, engine_a, sink, input_block, output_block), signal_lab::engine_failure::clipping) && sink.frames == 4U && sink.calls == 1U &&
              signal_lab::same_failure(engine_a.status(), signal_lab::engine_failure::clipping),
          "pipeline propagates clipping and does not deliver the rejected block");
    source.cursor = 0U;
    check(signal_lab::same_failure(signal_lab::run_pipeline(source, engine_a, sink, input_block, output_block), signal_lab::engine_failure::clipping) && source.cursor == 0U,
          "a pipeline on a failed engine returns its latched Status and reads nothing");
    source.cursor = 0U;
    source.length = 4U;
    sink.frames = sink.calls = 0U;
    scenario.source_gain_db = 0.0;
    scenario.stage_count = 1U;
    scenario.stages[0].type = signal_lab::StageType::sample_slip;
    scenario.stages[0].slip.event_count = 1U;
    scenario.stages[0].slip.events[0] = {0.0, false, 4U};
    check(engine_a.configure(scenario, 0.3, source.total_frames()).is_ok(), "pipeline deletion scenario configures");
    check(signal_lab::run_pipeline(source, engine_a, sink, input_block, output_block).is_ok() && sink.frames == 0U && sink.calls == 0U && engine_a.stats().frames_in == 4U && engine_a.status().is_ok(),
          "pipeline accepts a successful zero-output deletion block");
    // A source's failure is the pipeline's, never the end of the stream: the
    // blocks before it are delivered, and the engine is not failed by it.
    constexpr m110::Status unreadable{.code = m110::StatusCode::io_error, .message = "the test source cannot read"};
    constexpr std::size_t first_block = 4U;
    constexpr double reference_rms = 0.3;
    std::array<std::int16_t, signal_lab::engine_block_frames> input{};
    std::array<std::int16_t, signal_lab::engine_capacity_frames> output{};
    source.cursor = 0U;
    source.length = 2U * first_block;
    source.failing_at = first_block;
    source.failure = unreadable;
    sink.frames = sink.calls = 0U;
    check(engine_a.configure(signal_lab::Scenario{}, reference_rms, source.total_frames()).is_ok(), "pipeline pass-through scenario configures");
    check(signal_lab::same_failure(signal_lab::run_pipeline(source, engine_a, sink, input.data(), output.data()), unreadable) && sink.frames == first_block && sink.calls == 1U &&
              engine_a.stats().frames_in == first_block && engine_a.status().is_ok(),
          "a source that fails after a block ends the pipeline with its Status, the block delivered");
    source.cursor = 0U;
    source.failing_at = 0U;
    sink.frames = sink.calls = 0U;
    check(engine_a.configure(signal_lab::Scenario{}, reference_rms, source.total_frames()).is_ok(), "pipeline pass-through scenario configures again");
    check(signal_lab::same_failure(signal_lab::run_pipeline(source, engine_a, sink, input.data(), output.data()), unreadable) && sink.calls == 0U && engine_a.stats().frames_in == 0U,
          "a source that fails at once ends the pipeline with its Status before any block");
}

// Engine::configure()'s refusals are Statuses: the code says which kind, the
// static message which rule, and the engine is left unconfigured.
void test_configure_status()
{
    signal_lab::Scenario scenario;
    const m110::Status silent = engine_a.configure(scenario, 0.0, 4U);
    check(silent.code == m110::StatusCode::invalid_argument && std::strcmp(silent.message, "reference RMS must be positive") == 0 && !engine_a.configured(),
          "a reference RMS of 0 is invalid_argument");
    const std::string zero_fade = R"({"impairments":[{"type":"fade","depth_db":6,"duration_ms":0,"period_seconds":1}]})";
    check(signal_lab::parse_scenario(zero_fade.c_str(), zero_fade.size(), scenario).is_ok(), "a 0 ms fade passes validation");
    const m110::Status fade = engine_a.configure(scenario, 0.3, 48000U);
    check(fade.code == m110::StatusCode::invalid_configuration && std::strcmp(fade.message, "fade duration must span at least one sample") == 0 && !engine_a.configured() &&
              engine_a.stage(0U) == nullptr,
          "a fade's prepare() refusal is the stage's Status, and no stage is kept");
    // max_stages impulse stages: more than the arena holds.
    constexpr double impulse_decay_ms = 1.0;
    constexpr double impulse_truncate_tau = 4.0;
    constexpr double impulse_period_seconds = 1.0;
    check(signal_lab::max_stages * signal_lab::impulse_size() > signal_lab::impairment_arena_bytes, "max_stages impulse stages overflow the arena");
    scenario = signal_lab::Scenario{};
    scenario.stage_count = signal_lab::max_stages;

    for (signal_lab::Stage& stage : std::span{scenario.stages})
    {
        stage.type = signal_lab::StageType::impulse;
        stage.impulse.decay_ms = impulse_decay_ms;
        stage.impulse.truncate_tau = impulse_truncate_tau;
        stage.impulse.period_seconds = impulse_period_seconds;
    }

    const m110::Status full = engine_a.configure(scenario, 0.3, 48000U);
    check(full.code == m110::StatusCode::buffer_too_small && std::strcmp(full.message, "impairment arena exhausted") == 0 && !engine_a.configured(),
          "a scenario that overflows the arena is buffer_too_small");
}

} // namespace

int main()
{
    test_det_math();
    test_json();
    test_json_numbers();
    test_json_refusals();
    test_json_structure();
    test_scenario();
    test_engine_determinism();
    test_levels();
    test_slips();
    test_slip_validation_and_capacity();
    test_fade_event_boundaries();
    test_processing_failures_and_emitted_levels();
    test_pipeline_error_propagation();
    test_configure_status();

    if (failures == 0)
    {
        std::printf("signal_lab tests: PASS\n");
        return 0;
    }

    std::printf("signal_lab tests: %d FAILURES\n", failures);
    return 1;
}
