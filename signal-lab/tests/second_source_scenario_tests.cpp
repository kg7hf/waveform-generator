// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// Scenario schema and placement for the one bounded secondary source.

#include "signal_lab/scenario.hpp"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace
{

namespace sl = signal_lab;

[[nodiscard]] m110::Status parse(std::string_view text, sl::Scenario& scenario)
{
    return sl::parse_scenario(text.data(), text.size(), scenario);
}

[[nodiscard]] std::string document(std::string_view stage)
{
    return std::string{"{\"impairments\":["} + std::string{stage} + "]}";
}

} // namespace

TEST_CASE("second source: schema fields, defaults and frame limits", "[second_source][scenario]")
{
    auto scenario = std::make_unique<sl::Scenario>();
    REQUIRE(parse(document(R"({"type":"second_source","source_path":"interferer.wav","si_db":12})"), *scenario).is_ok());
    CHECK(scenario->stages[0].type == sl::StageType::second_source);
    CHECK(std::string_view{sl::stage_type_name(sl::StageType::second_source)} == "second_source");
    CHECK(std::string_view{scenario->second_source_path} == "interferer.wav");
    CHECK(scenario->second_source.si.in(sl::units::dB) == 12.0);
    CHECK(scenario->second_source.start_frame == 0U);
    CHECK(scenario->second_source.source_start_frame == 0U);
    CHECK(scenario->second_source.duration_frames == 0U);
    CHECK(scenario->second_source.frequency_offset.hertz() == 0.0);
    CHECK_FALSE(scenario->second_source.fading.enabled);
    CHECK_FALSE(scenario->second_source.markov.enabled);
    constexpr std::uint64_t maximum = sl::max_second_source_frames;
    const std::string boundary = std::string{R"({"type":"second_source","source_path":"i.wav","si_db":-120,"start_frame":)"} + std::to_string(maximum - 4U) +
                                 R"(,"source_start_frame":2,"duration_frames":2,"frequency_offset_hz":-75})";
    REQUIRE(parse(document(boundary), *scenario).is_ok());
    CHECK(scenario->second_source.start_frame == maximum - 4U);
    CHECK(scenario->second_source.source_start_frame == 2U);
    CHECK(scenario->second_source.duration_frames == 2U);
    CHECK(scenario->second_source.frequency_offset.hertz() == -75.0);
    const std::string top_boundary = std::string{R"({"type":"second_source","source_path":"i.wav","si_db":120,"start_frame":)"} + std::to_string(maximum - 1U) + R"(,"source_start_frame":)" +
                                     std::to_string(maximum - 1U) + R"(,"duration_frames":1,"frequency_offset_hz":75})";
    CHECK(parse(document(top_boundary), *scenario).is_ok());
}

TEST_CASE("second source: strict malformed fields are refused", "[second_source][scenario]")
{
    auto scenario = std::make_unique<sl::Scenario>();
    constexpr std::array refused{
        R"({"type":"second_source","source_path":"i.wav"})",
        R"({"type":"second_source","source_path":"","si_db":0})",
        R"({"type":"second_source","source_path":null,"si_db":0})",
        R"({"type":"second_source","source_path":4,"si_db":0})",
        R"({"type":"second_source","source_path":"i.wav","si_db":true})",
        R"({"type":"second_source","source_path":"i.wav","si_db":0,"other":1})",
        R"({"type":"second_source","source_path":"i.wav","si_db":0,"si_db":1})",
        R"({"type":"second_source","source_path":"i.wav","si_db":0,"source_path":"j.wav"})",
        R"({"type":"second_source","source_path":"i.wav","si_db":0,"start_frame":true})",
        R"({"type":"second_source","source_path":"i.wav","si_db":0,"start_frame":-1})",
        R"({"type":"second_source","source_path":"i.wav","si_db":0,"start_frame":1.5})",
        R"({"type":"second_source","source_path":"i.wav","si_db":0,"start_frame":48000000000001})",
        R"({"type":"second_source","source_path":"i.wav","si_db":0,"source_start_frame":null})",
        R"({"type":"second_source","source_path":"i.wav","si_db":0,"duration_frames":0})",
        R"({"type":"second_source","source_path":"i.wav","si_db":0,"duration_frames":-1})",
        R"({"type":"second_source","source_path":"i.wav","si_db":0,"duration_frames":1.25})",
        R"({"type":"second_source","source_path":"i.wav","si_db":0,"duration_frames":48000000000001})",
        R"({"type":"second_source","source_path":"i.wav","si_db":0,"frequency_offset_hz":76})",
        R"({"type":"second_source","source_path":"i.wav","si_db":0,"frequency_offset_hz":"1"})",
    };

    for (const auto* stage : refused)
    {
        CAPTURE(stage);
        CHECK_FALSE(parse(document(stage), *scenario).is_ok());
    }

    CHECK_FALSE(parse(document(R"({"type":"second_source","source_path":"i.wav","si_db":0,"start_frame":1,"start_frame":2})"), *scenario).is_ok());
    CHECK_FALSE(parse(document(R"({"type":"second_source","source_path":"i.wav","si_db":0,"duration_frames":1,"duration_frames":2})"), *scenario).is_ok());
    CHECK_FALSE(parse(document(R"({"type":"second_source","source_path":"i.wav","si_db":0,"frequency_offset_hz":NaN})"), *scenario).is_ok());
    CHECK_FALSE(parse(document(R"({"type":"second_source","source_path":"bad\u0000path","si_db":0})"), *scenario).is_ok());
    CHECK_FALSE(parse(document(R"({"type":"second_source","source_path":"caf\u00E9.wav","si_db":0})"), *scenario).is_ok());
    std::string invalid_utf8 = R"({"type":"second_source","source_path":"bad)";
    invalid_utf8.push_back(static_cast<char>(0xC0U));
    invalid_utf8.push_back(static_cast<char>(0xAFU));
    invalid_utf8 += R"(","si_db":0})";
    CHECK(static_cast<unsigned char>(invalid_utf8[invalid_utf8.find("bad") + 3U]) == 0xC0U);
    CHECK_FALSE(parse(document(invalid_utf8), *scenario).is_ok());
}

TEST_CASE("second source: path capacity and typed descriptors", "[second_source][scenario]")
{
    auto scenario = std::make_unique<sl::Scenario>();
    const std::string max_path(sl::second_source_path_capacity - 1U, 'a');
    const std::string accepted = std::string{R"({"type":"second_source","source_path":")"} + max_path + R"(","si_db":0})";
    CHECK(parse(document(accepted), *scenario).is_ok());
    const std::string too_long = std::string{R"({"type":"second_source","source_path":")"} + std::string(sl::second_source_path_capacity, 'a') + R"(","si_db":0})";
    CHECK_FALSE(parse(document(too_long), *scenario).is_ok());
    const std::string raw_utf8 = std::string{R"({"type":"second_source","source_path":"cafe)"} + "\xC3\xA9" + R"(.wav","si_db":0})";
    CHECK(parse(raw_utf8, *scenario).is_ok());
    REQUIRE(parse(document(R"({"type":"second_source","source_path":"i.wav","si_db":0})"), *scenario).is_ok());
    scenario->second_source.si = sl::units::Ratio{121.0, sl::units::dB};
    CHECK_FALSE(sl::validate_scenario(*scenario).is_ok());
    REQUIRE(parse(document(R"({"type":"second_source","source_path":"i.wav","si_db":0})"), *scenario).is_ok());
    scenario->second_source.frequency_offset = sl::units::FrequencyDelta{76.0, sl::units::Hz};
    CHECK_FALSE(sl::validate_scenario(*scenario).is_ok());
    REQUIRE(parse(document(R"({"type":"second_source","source_path":"i.wav","si_db":0})"), *scenario).is_ok());
    scenario->second_source.start_frame = sl::max_second_source_frames + 1U;
    CHECK_FALSE(sl::validate_scenario(*scenario).is_ok());
}

TEST_CASE("second source: one slot follows channel and shift before other stages", "[second_source][scenario]")
{
    auto scenario = std::make_unique<sl::Scenario>();
    constexpr std::string_view valid =
        R"({"impairments":[{"type":"watterson","paths":[{"fading":"fixed"}]},{"type":"frequency_shift","mode":"offset","offset_hz":0},{"type":"second_source","source_path":"i.wav","si_db":6},{"type":"awgn","snr_db":24}]})";
    REQUIRE(parse(valid, *scenario).is_ok());
    CHECK(scenario->stages[2].type == sl::StageType::second_source);
    std::swap(scenario->stages[1], scenario->stages[2]);
    CHECK_FALSE(sl::validate_scenario(*scenario).is_ok());
    REQUIRE(parse(valid, *scenario).is_ok());
    std::swap(scenario->stages[0], scenario->stages[2]);
    CHECK_FALSE(sl::validate_scenario(*scenario).is_ok());
    constexpr std::string_view shift_then_source =
        R"({"impairments":[{"type":"frequency_shift","mode":"offset","offset_hz":0},{"type":"second_source","source_path":"i.wav","si_db":6},{"type":"cw","frequency_hz":1200,"ci_db":20}]})";
    REQUIRE(parse(shift_then_source, *scenario).is_ok());
    std::swap(scenario->stages[0], scenario->stages[1]);
    CHECK_FALSE(sl::validate_scenario(*scenario).is_ok());
    constexpr std::string_view duplicate = R"({"impairments":[{"type":"second_source","source_path":"a.wav","si_db":6},{"type":"second_source","source_path":"b.wav","si_db":12}]})";
    CHECK_FALSE(parse(duplicate, *scenario).is_ok());
}

TEST_CASE("second source: independent fading grammar is optional and strict", "[second_source][scenario][fading]")
{
    auto scenario = std::make_unique<sl::Scenario>();
    constexpr std::string_view prefix = R"({"type":"second_source","stream_id":42,"source_path":"i.wav","si_db":12,)";

    for (const auto* fading : {R"("fading":{"spread_hz":0.1})", R"("fading":{"spread_hz":10})"})
    {
        REQUIRE(parse(document(std::string{prefix} + fading + '}'), *scenario).is_ok());
        CHECK(scenario->second_source.fading.enabled);
        CHECK(scenario->stages[0].stream_id == 42U);
        CHECK(sl::second_source_filter_latency_frames(scenario->second_source).value() == sl::hilbert_delay);
    }

    constexpr std::array refused{
        R"("fading":null)",
        R"("fading":false)",
        R"("fading":1)",
        R"("fading":[])",
        R"("fading":{})",
        R"("fading":{"spread_hz":true})",
        R"("fading":{"spread_hz":"1"})",
        R"("fading":{"spread_hz":null})",
        R"("fading":{"spread_hz":0})",
        R"("fading":{"spread_hz":0.099})",
        R"("fading":{"spread_hz":10.001})",
        R"("fading":{"spread_hz":-1})",
        R"("fading":{"spread_hz":1,"other":0})",
        R"("fading":{"other":1})",
        R"("fading":{"spread_hz":1,"spread_hz":2})",
        R"("fading":{"spread_hz":1},"fading":{"spread_hz":2})",
    };

    for (const auto* fading : refused)
    {
        CAPTURE(fading);
        CHECK_FALSE(parse(document(std::string{prefix} + fading + '}'), *scenario).is_ok());
    }

    REQUIRE(parse(document(std::string{prefix} + R"("fading":{"spread_hz":1}})"), *scenario).is_ok());
    REQUIRE(parse(document(R"({"type":"second_source","source_path":"i.wav","si_db":12})"), *scenario).is_ok());
    CHECK_FALSE(scenario->second_source.fading.enabled);
    CHECK(sl::second_source_filter_latency_frames(scenario->second_source).value() == 0U);
}

TEST_CASE("second source: Markov gate grammar is optional, strict and filter latency neutral", "[second_source][scenario][markov]")
{
    auto scenario = std::make_unique<sl::Scenario>();
    constexpr std::string_view stage = R"({"type":"second_source","source_path":"i.wav","si_db":12,"markov_on_off":{"mean_on_seconds":0.001,"mean_off_seconds":0.002}})";
    REQUIRE(parse(document(stage), *scenario).is_ok());
    CHECK(scenario->second_source.markov.enabled);
    CHECK(scenario->second_source.markov.mean_on_seconds == 0.001);
    CHECK(scenario->second_source.markov.mean_off_seconds == 0.002);
    CHECK(sl::second_source_filter_latency_frames(scenario->second_source).value() == 0U);
    constexpr std::string_view combined =
        R"({"type":"second_source","source_path":"i.wav","si_db":12,"frequency_offset_hz":-75,"fading":{"spread_hz":1},"markov_on_off":{"mean_on_seconds":0.001,"mean_off_seconds":0.002}})";
    REQUIRE(parse(document(combined), *scenario).is_ok());
    CHECK(scenario->second_source.fading.enabled);
    CHECK(scenario->second_source.frequency_offset.in(sl::units::Hz) == -75.0);
    CHECK(scenario->second_source.markov.enabled);
    CHECK(sl::second_source_filter_latency_frames(scenario->second_source) == sl::hilbert_latency);
    constexpr std::string_view frame_boundary =
        R"({"type":"second_source","source_path":"i.wav","si_db":0,"markov_on_off":{"mean_on_seconds":2.0833333333333333e-5,"mean_off_seconds":2.0833333333333333e-5}})";
    CHECK(parse(document(frame_boundary), *scenario).is_ok());
    CHECK(scenario->second_source.markov.mean_on_seconds * sl::sample_rate_hz == 1.0);
    CHECK(scenario->second_source.markov.mean_off_seconds * sl::sample_rate_hz == 1.0);
    constexpr std::array refused{
        R"("markov_on_off":null)",
        R"("markov_on_off":false)",
        R"("markov_on_off":1)",
        R"("markov_on_off":[])",
        R"("markov_on_off":{})",
        R"("markov_on_off":{"mean_on_seconds":0.001})",
        R"("markov_on_off":{"mean_off_seconds":0.001})",
        R"("markov_on_off":{"mean_on_seconds":true,"mean_off_seconds":0.001})",
        R"("markov_on_off":{"mean_on_seconds":0.001,"mean_off_seconds":null})",
        R"("markov_on_off":{"mean_on_seconds":"0.001","mean_off_seconds":0.001})",
        R"("markov_on_off":{"mean_on_seconds":0.5,"mean_off_seconds":0.001,"future":1})",
        R"("markov_on_off":{"mean_on_seconds":0,"mean_off_seconds":0.001})",
        R"("markov_on_off":{"mean_on_seconds":0.000010416666666666667,"mean_off_seconds":0.001})",
        R"("markov_on_off":{"mean_on_seconds":1e309,"mean_off_seconds":0.001})",
        R"("markov_on_off":{"mean_on_seconds":0.001,"mean_off_seconds":1e309})",
        R"("markov_on_off":{"mean_on_seconds":0.001,"mean_on_seconds":0.002,"mean_off_seconds":0.001})",
        R"("markov_on_off":{"mean_on_seconds":0.001,"mean_off_seconds":0.001,"mean_off_seconds":0.002})",
    };
    constexpr std::string_view prefix = R"({"type":"second_source","source_path":"i.wav","si_db":0,)";

    for (const std::string_view fields : refused)
    {
        CAPTURE(fields);
        CHECK_FALSE(parse(document(std::string{prefix} + std::string{fields} + '}'), *scenario).is_ok());
    }

    CHECK_FALSE(
        parse(document(std::string{prefix} + R"("markov_on_off":{"mean_on_seconds":0.001,"mean_off_seconds":0.001},"markov_on_off":{"mean_on_seconds":0.002,"mean_off_seconds":0.002}})"), *scenario)
            .is_ok());
    REQUIRE(parse(document(stage), *scenario).is_ok());
    scenario->second_source.markov.mean_off_seconds = std::numeric_limits<double>::quiet_NaN();
    CHECK_FALSE(sl::validate_scenario(*scenario).is_ok());
    REQUIRE(parse(document(stage), *scenario).is_ok());
    scenario->second_source.markov.mean_on_seconds = 0.5 / static_cast<double>(sl::sample_rate_hz);
    CHECK_FALSE(sl::validate_scenario(*scenario).is_ok());
}
