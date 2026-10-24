// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// CW Markov gate scenario grammar and typed-descriptor validation.

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
    return std::string{R"({"impairments":[{"type":"cw","frequency_hz":1200,"ci_db":0)"} + std::string{fields} + R"(}]})";
}

} // namespace

TEST_CASE("CW Markov scenario: absent gate remains disabled and fractional means are accepted", "[cw][markov][scenario]")
{
    auto scenario = std::make_unique<sl::Scenario>();
    REQUIRE(parse(cw_scenario(""), *scenario).is_ok());
    CHECK_FALSE(scenario->stages[0].cw.markov.enabled);
    REQUIRE(parse(cw_scenario(R"(,"markov_on_off":{"mean_on_seconds":0.001001,"mean_off_seconds":0.002003})"), *scenario).is_ok());
    CHECK(scenario->stages[0].cw.markov.enabled);
    CHECK(scenario->stages[0].cw.markov.mean_on_seconds == 0.001001);
    CHECK(scenario->stages[0].cw.markov.mean_off_seconds == 0.002003);
    CHECK(sl::validate_scenario(*scenario).is_ok());
}

TEST_CASE("CW Markov scenario: means accept inclusive frame and duration bounds", "[cw][markov][scenario]")
{
    auto scenario = std::make_unique<sl::Scenario>();
    REQUIRE(parse(cw_scenario(R"(,"markov_on_off":{"mean_on_seconds":0.000020833333333333333,"mean_off_seconds":1000000})"), *scenario).is_ok());
    CHECK(sl::validate_scenario(*scenario).is_ok());
}

TEST_CASE("CW Markov scenario: object and mean fields are strict", "[cw][markov][scenario]")
{
    constexpr std::array refused{
        R"({"impairments":[{"type":"cw","frequency_hz":1200,"ci_db":0,"markov_on_off":null}]})",
        R"({"impairments":[{"type":"cw","frequency_hz":1200,"ci_db":0,"markov_on_off":{}}]})",
        R"({"impairments":[{"type":"cw","frequency_hz":1200,"ci_db":0,"markov_on_off":{"mean_on_seconds":1}}]})",
        R"({"impairments":[{"type":"cw","frequency_hz":1200,"ci_db":0,"markov_on_off":{"mean_off_seconds":1}}]})",
        R"({"impairments":[{"type":"cw","frequency_hz":1200,"ci_db":0,"markov_on_off":{"mean_on_seconds":1,"mean_off_seconds":1,"future":1}}]})",
        R"({"impairments":[{"type":"cw","frequency_hz":1200,"ci_db":0,"markov_on_off":{"mean_on_seconds":1,"mean_on_seconds":2,"mean_off_seconds":1}}]})",
        R"({"impairments":[{"type":"cw","frequency_hz":1200,"ci_db":0,"markov_on_off":{"mean_on_seconds":1,"mean_off_seconds":1,"mean_off_seconds":2}}]})",
        R"({"impairments":[{"type":"cw","frequency_hz":1200,"ci_db":0,"markov_on_off":{"mean_on_seconds":true,"mean_off_seconds":1}}]})",
        R"({"impairments":[{"type":"cw","frequency_hz":1200,"ci_db":0,"markov_on_off":{"mean_on_seconds":1,"mean_off_seconds":null}}]})",
        R"({"impairments":[{"type":"cw","frequency_hz":1200,"ci_db":0,"markov_on_off":{"mean_on_seconds":1e309,"mean_off_seconds":1}}]})",
        R"({"impairments":[{"type":"cw","frequency_hz":1200,"ci_db":0,"markov_on_off":{"mean_on_seconds":1,"mean_off_seconds":"2"}}]})",
        R"({"impairments":[{"type":"cw","frequency_hz":1200,"ci_db":0,"markov_on_off":{"mean_on_seconds":1,"mean_off_seconds":1},"markov_on_off":{"mean_on_seconds":2,"mean_off_seconds":2}}]})",
    };
    auto scenario = std::make_unique<sl::Scenario>();

    for (const std::string_view text : refused)
    {
        CAPTURE(text);
        CHECK_FALSE(parse(text, *scenario).is_ok());
    }
}

TEST_CASE("CW Markov scenario: means outside supported durations are refused", "[cw][markov][scenario]")
{
    constexpr std::array refused{
        R"({"impairments":[{"type":"cw","frequency_hz":1200,"ci_db":0,"markov_on_off":{"mean_on_seconds":0,"mean_off_seconds":1}}]})",
        R"({"impairments":[{"type":"cw","frequency_hz":1200,"ci_db":0,"markov_on_off":{"mean_on_seconds":0.0000208,"mean_off_seconds":1}}]})",
        R"({"impairments":[{"type":"cw","frequency_hz":1200,"ci_db":0,"markov_on_off":{"mean_on_seconds":1,"mean_off_seconds":1000000.0001}}]})",
        R"({"impairments":[{"type":"cw","frequency_hz":1200,"ci_db":0,"markov_on_off":{"mean_on_seconds":-1,"mean_off_seconds":1}}]})",
    };
    auto scenario = std::make_unique<sl::Scenario>();

    for (const std::string_view text : refused)
    {
        CAPTURE(text);
        CHECK_FALSE(parse(text, *scenario).is_ok());
    }
}

TEST_CASE("CW Markov scenario: sweep can combine but periodic pulse cannot", "[cw][markov][scenario]")
{
    auto scenario = std::make_unique<sl::Scenario>();
    constexpr std::string_view swept = R"({"impairments":[{"type":"cw","frequency_sweep":{"low_hz":500,"high_hz":1800,)"
                                       R"("slope_hz_per_second":20},"ci_db":0,"markov_on_off":{"mean_on_seconds":0.5,)"
                                       R"("mean_off_seconds":0.25}}]})";
    REQUIRE(parse(swept, *scenario).is_ok());
    CHECK(scenario->stages[0].cw.sweep.enabled);
    CHECK(scenario->stages[0].cw.markov.enabled);
    CHECK_FALSE(parse(cw_scenario(R"(,"markov_on_off":{"mean_on_seconds":1,"mean_off_seconds":2},"pulse_period_seconds":1,"pulse_duty_cycle":0.5)"), *scenario).is_ok());
}

TEST_CASE("CW Markov scenario: programmatic descriptors reject periodic pulse ambiguity", "[cw][markov][scenario]")
{
    auto scenario = std::make_unique<sl::Scenario>();
    REQUIRE(parse(cw_scenario(R"(,"markov_on_off":{"mean_on_seconds":1,"mean_off_seconds":2})"), *scenario).is_ok());
    scenario->stages[0].cw.pulse_period_frames = 48000U;
    scenario->stages[0].cw.pulse_on_frames = 24000U;
    CHECK_FALSE(sl::validate_scenario(*scenario).is_ok());
}

TEST_CASE("CW Markov scenario: programmatic descriptors validate enabled means", "[cw][markov][scenario]")
{
    auto scenario = std::make_unique<sl::Scenario>();
    REQUIRE(parse(cw_scenario(R"(,"markov_on_off":{"mean_on_seconds":1,"mean_off_seconds":2})"), *scenario).is_ok());
    scenario->stages[0].cw.markov.mean_on_seconds = 0.0;
    CHECK_FALSE(sl::validate_scenario(*scenario).is_ok());
}
