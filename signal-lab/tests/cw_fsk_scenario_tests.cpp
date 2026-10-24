// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// CW FSK scenario grammar and typed-descriptor validation.

#include "signal_lab/scenario.hpp"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <string>
#include <string_view>

namespace
{

namespace sl = signal_lab;

[[nodiscard]] m110::Status parse(std::string_view text, sl::Scenario& scenario)
{
    return sl::parse_scenario(text.data(), text.size(), scenario);
}

[[nodiscard]] std::string cw_scenario(std::string_view fields)
{
    return std::string{R"({"impairments":[{"type":"cw","frequency_hz":12000,"ci_db":0)"} + std::string{fields} + R"(}]})";
}

} // namespace

TEST_CASE("CW FSK scenario: omitted object keeps fixed CW disabled by default", "[cw][fsk][scenario]")
{
    auto scenario = std::make_unique<sl::Scenario>();
    REQUIRE(parse(cw_scenario(""), *scenario).is_ok());
    const sl::CwParams& cw = scenario->stages[0].cw;
    CHECK_FALSE(cw.fsk.enabled);
    CHECK(cw.fsk.baud == 0.0);
    CHECK(cw.fsk.shift.in(sl::units::Hz) == 0.0);
    CHECK(cw.fsk.pattern == sl::FskPattern::alternating);
    CHECK(sl::validate_scenario(*scenario).is_ok());
}

TEST_CASE("CW FSK scenario: full tone separation and exact fractional baud parse", "[cw][fsk][scenario]")
{
    auto scenario = std::make_unique<sl::Scenario>();
    REQUIRE(parse(cw_scenario(R"(,"fsk":{"baud":37.5,"shift_hz":300,"pattern":"alternating"})"), *scenario).is_ok());
    const sl::CwParams& cw = scenario->stages[0].cw;
    CHECK(cw.fsk.enabled);
    CHECK(cw.fsk.baud == 37.5);
    CHECK(cw.fsk.shift.in(sl::units::Hz) == 300.0);
    CHECK(cw.fsk.pattern == sl::FskPattern::alternating);
    const auto symbol_frames = sl::cw_fsk_symbol_frames(cw.fsk);
    REQUIRE(symbol_frames.has_value());
    CHECK(symbol_frames.value() == 1280U);
    CHECK(sl::validate_scenario(*scenario).is_ok());
}

TEST_CASE("CW FSK scenario: boundaries include one-frame baud and maximum symbol period", "[cw][fsk][scenario]")
{
    auto scenario = std::make_unique<sl::Scenario>();
    REQUIRE(parse(cw_scenario(R"(,"fsk":{"baud":48000,"shift_hz":300,"pattern":"random"})"), *scenario).is_ok());
    auto symbol_frames = sl::cw_fsk_symbol_frames(scenario->stages[0].cw.fsk);
    REQUIRE(symbol_frames.has_value());
    CHECK(symbol_frames.value() == 1U);
    REQUIRE(parse(cw_scenario(R"(,"fsk":{"baud":1e-9,"shift_hz":300,"pattern":"random"})"), *scenario).is_ok());
    symbol_frames = sl::cw_fsk_symbol_frames(scenario->stages[0].cw.fsk);
    REQUIRE(symbol_frames.has_value());
    CHECK(symbol_frames.value() == sl::cw_fsk_max_symbol_frames);
}

TEST_CASE("CW FSK scenario: strict nested fields and top-level uniqueness", "[cw][fsk][scenario]")
{
    constexpr std::array refused{
        R"({"impairments":[{"type":"cw","frequency_hz":12000,"ci_db":0,"fsk":null}]})",
        R"({"impairments":[{"type":"cw","frequency_hz":12000,"ci_db":0,"fsk":{}}]})",
        R"({"impairments":[{"type":"cw","frequency_hz":12000,"ci_db":0,"fsk":{"shift_hz":300,"pattern":"random"}}]})",
        R"({"impairments":[{"type":"cw","frequency_hz":12000,"ci_db":0,"fsk":{"baud":300,"pattern":"random"}}]})",
        R"({"impairments":[{"type":"cw","frequency_hz":12000,"ci_db":0,"fsk":{"baud":300,"shift_hz":300}}]})",
        R"({"impairments":[{"type":"cw","frequency_hz":12000,"ci_db":0,"fsk":{"baud":300,"shift_hz":300,"pattern":"random","future":1}}]})",
        R"({"impairments":[{"type":"cw","frequency_hz":12000,"ci_db":0,"fsk":{"baud":300,"baud":600,"shift_hz":300,"pattern":"random"}}]})",
        R"({"impairments":[{"type":"cw","frequency_hz":12000,"ci_db":0,"fsk":{"baud":300,"shift_hz":300,"shift_hz":400,"pattern":"random"}}]})",
        R"({"impairments":[{"type":"cw","frequency_hz":12000,"ci_db":0,"fsk":{"baud":300,"shift_hz":300,"pattern":"random","pattern":"alternating"}}]})",
        R"({"impairments":[{"type":"cw","frequency_hz":12000,"ci_db":0,"fsk":{"baud":true,"shift_hz":300,"pattern":"random"}}]})",
        R"({"impairments":[{"type":"cw","frequency_hz":12000,"ci_db":0,"fsk":{"baud":300,"shift_hz":false,"pattern":"random"}}]})",
        R"({"impairments":[{"type":"cw","frequency_hz":12000,"ci_db":0,"fsk":{"baud":300,"shift_hz":300,"pattern":true}}]})",
        R"({"impairments":[{"type":"cw","frequency_hz":12000,"ci_db":0,"fsk":{"baud":1e309,"shift_hz":300,"pattern":"random"}}]})",
        R"({"impairments":[{"type":"cw","frequency_hz":12000,"ci_db":0,"fsk":{"baud":300,"shift_hz":300,"pattern":"other"}}]})",
        R"({"impairments":[{"type":"cw","frequency_hz":12000,"ci_db":0,"fsk":{"baud":300,"shift_hz":300,"pattern":"random"},"fsk":{"baud":300,"shift_hz":300,"pattern":"random"}}]})",
    };
    auto scenario = std::make_unique<sl::Scenario>();

    for (const std::string_view text : refused)
    {
        CAPTURE(text);
        CHECK_FALSE(parse(text, *scenario).is_ok());
    }
}

TEST_CASE("CW FSK scenario: invalid baud symbol period and tone edges refuse", "[cw][fsk][scenario]")
{
    constexpr std::array refused{
        R"({"impairments":[{"type":"cw","frequency_hz":12000,"ci_db":0,"fsk":{"baud":0,"shift_hz":300,"pattern":"random"}}]})",
        R"({"impairments":[{"type":"cw","frequency_hz":12000,"ci_db":0,"fsk":{"baud":-1,"shift_hz":300,"pattern":"random"}}]})",
        R"({"impairments":[{"type":"cw","frequency_hz":12000,"ci_db":0,"fsk":{"baud":110,"shift_hz":300,"pattern":"random"}}]})",
        R"({"impairments":[{"type":"cw","frequency_hz":12000,"ci_db":0,"fsk":{"baud":0.000000000999,"shift_hz":300,"pattern":"random"}}]})",
        R"({"impairments":[{"type":"cw","frequency_hz":12000,"ci_db":0,"fsk":{"baud":300,"shift_hz":0,"pattern":"random"}}]})",
        R"({"impairments":[{"type":"cw","frequency_hz":12000,"ci_db":0,"fsk":{"baud":300,"shift_hz":-1,"pattern":"random"}}]})",
        R"({"impairments":[{"type":"cw","frequency_hz":1500,"ci_db":0,"fsk":{"baud":300,"shift_hz":2999,"pattern":"random"}}]})",
        R"({"impairments":[{"type":"cw","frequency_hz":22500,"ci_db":0,"fsk":{"baud":300,"shift_hz":3000,"pattern":"random"}}]})",
    };
    auto scenario = std::make_unique<sl::Scenario>();

    for (const std::string_view text : refused)
    {
        CAPTURE(text);
        CHECK_FALSE(parse(text, *scenario).is_ok());
    }
}

TEST_CASE("CW FSK scenario: inclusive 1 Hz and 23999 Hz tones are accepted", "[cw][fsk][scenario]")
{
    auto scenario = std::make_unique<sl::Scenario>();
    REQUIRE(parse(R"({"impairments":[{"type":"cw","frequency_hz":1500,"ci_db":0,"fsk":{"baud":300,"shift_hz":2998,"pattern":"alternating"}}]})", *scenario).is_ok());
    CHECK(sl::validate_scenario(*scenario).is_ok());
    REQUIRE(parse(R"({"impairments":[{"type":"cw","frequency_hz":22500,"ci_db":0,"fsk":{"baud":300,"shift_hz":2998,"pattern":"random"}}]})", *scenario).is_ok());
    CHECK(sl::validate_scenario(*scenario).is_ok());
}

TEST_CASE("CW FSK scenario: fixed center is required and sweep is incompatible", "[cw][fsk][scenario]")
{
    constexpr std::array refused{
        R"({"impairments":[{"type":"cw","ci_db":0,"fsk":{"baud":300,"shift_hz":300,"pattern":"random"}}]})",
        R"({"impairments":[{"type":"cw","frequency_sweep":{"low_hz":1000,"high_hz":2000,"slope_hz_per_second":10},"ci_db":0,"fsk":{"baud":300,"shift_hz":300,"pattern":"random"}}]})",
    };
    auto scenario = std::make_unique<sl::Scenario>();

    for (const std::string_view text : refused)
    {
        CAPTURE(text);
        CHECK_FALSE(parse(text, *scenario).is_ok());
    }
}

TEST_CASE("CW FSK scenario: periodic or Markov gates can combine with the FSK carrier", "[cw][fsk][scenario]")
{
    auto scenario = std::make_unique<sl::Scenario>();
    REQUIRE(parse(cw_scenario(R"(,"fsk":{"baud":300,"shift_hz":300,"pattern":"alternating"},)"
                              R"("pulse_period_seconds":0.01,"pulse_duty_cycle":0.5)"),
                  *scenario)
                .is_ok());
    CHECK(scenario->stages[0].cw.fsk.enabled);
    CHECK(scenario->stages[0].cw.pulse_period_frames == 480U);
    CHECK(sl::validate_scenario(*scenario).is_ok());
    REQUIRE(parse(cw_scenario(R"(,"fsk":{"baud":300,"shift_hz":300,"pattern":"random"},)"
                              R"("markov_on_off":{"mean_on_seconds":1,"mean_off_seconds":2})"),
                  *scenario)
                .is_ok());
    CHECK(scenario->stages[0].cw.fsk.pattern == sl::FskPattern::random);
    CHECK(scenario->stages[0].cw.markov.enabled);
    CHECK(sl::validate_scenario(*scenario).is_ok());
}

TEST_CASE("CW FSK scenario: typed descriptors reject sweep conflicts and malformed params", "[cw][fsk][scenario]")
{
    auto scenario = std::make_unique<sl::Scenario>();
    REQUIRE(parse(cw_scenario(R"(,"fsk":{"baud":300,"shift_hz":300,"pattern":"random"})"), *scenario).is_ok());
    scenario->stages[0].cw.fsk.shift = sl::units::FrequencyDelta::from_hz(0.0);
    CHECK_FALSE(sl::validate_scenario(*scenario).is_ok());
    REQUIRE(parse(cw_scenario(R"(,"fsk":{"baud":300,"shift_hz":300,"pattern":"random"})"), *scenario).is_ok());
    scenario->stages[0].cw.sweep.enabled = true;
    CHECK_FALSE(sl::validate_scenario(*scenario).is_ok());
}
