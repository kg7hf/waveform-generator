// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// CW Rayleigh-fading scenario grammar and typed validation.

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

TEST_CASE("CW fading scenario: omission preserves the disabled default", "[cw][fading][scenario]")
{
    auto scenario = std::make_unique<sl::Scenario>();
    REQUIRE(parse(cw_scenario(""), *scenario).is_ok());
    const sl::CwFadingParams& fading = scenario->stages[0].cw.fading;
    CHECK_FALSE(fading.enabled);
    CHECK(fading.spread.in(sl::units::Hz) == 0.0);
    CHECK(sl::validate_scenario(*scenario).is_ok());
}

TEST_CASE("CW fading scenario: inclusive minimum and maximum spreads parse", "[cw][fading][scenario]")
{
    auto scenario = std::make_unique<sl::Scenario>();
    REQUIRE(parse(cw_scenario(R"(,"fading":{"spread_hz":0.1})"), *scenario).is_ok());
    CHECK(scenario->stages[0].cw.fading.enabled);
    CHECK(scenario->stages[0].cw.fading.spread.in(sl::units::Hz) == 0.1);
    CHECK(sl::validate_scenario(*scenario).is_ok());
    REQUIRE(parse(cw_scenario(R"(,"fading":{"spread_hz":10})"), *scenario).is_ok());
    CHECK(scenario->stages[0].cw.fading.spread.in(sl::units::Hz) == 10.0);
    CHECK(sl::validate_scenario(*scenario).is_ok());
}

TEST_CASE("CW fading scenario: nested shape and duplicate fields are strict", "[cw][fading][scenario]")
{
    constexpr std::array refused{
        R"({"impairments":[{"type":"cw","frequency_hz":12000,"ci_db":0,"fading":null}]})",
        R"({"impairments":[{"type":"cw","frequency_hz":12000,"ci_db":0,"fading":{}}]})",
        R"({"impairments":[{"type":"cw","frequency_hz":12000,"ci_db":0,"fading":{"future":1}}]})",
        R"({"impairments":[{"type":"cw","frequency_hz":12000,"ci_db":0,"fading":{"spread_hz":1,"future":1}}]})",
        R"({"impairments":[{"type":"cw","frequency_hz":12000,"ci_db":0,"fading":{"spread_hz":1,"spread_hz":2}}]})",
        R"({"impairments":[{"type":"cw","frequency_hz":12000,"ci_db":0,"fading":{"spread_hz":true}}]})",
        R"({"impairments":[{"type":"cw","frequency_hz":12000,"ci_db":0,"fading":{"spread_hz":null}}]})",
        R"({"impairments":[{"type":"cw","frequency_hz":12000,"ci_db":0,"fading":{"spread_hz":"1"}}]})",
        R"({"impairments":[{"type":"cw","frequency_hz":12000,"ci_db":0,"fading":{"spread_hz":1e309}}]})",
        R"({"impairments":[{"type":"cw","frequency_hz":12000,"ci_db":0,"fading":{"spread_hz":0}}]})",
        R"({"impairments":[{"type":"cw","frequency_hz":12000,"ci_db":0,"fading":{"spread_hz":0.099999}}]})",
        R"({"impairments":[{"type":"cw","frequency_hz":12000,"ci_db":0,"fading":{"spread_hz":10.000001}}]})",
        R"({"impairments":[{"type":"cw","frequency_hz":12000,"ci_db":0,"fading":{"spread_hz":1},"fading":{"spread_hz":2}}]})",
    };
    auto scenario = std::make_unique<sl::Scenario>();

    for (const std::string_view text : refused)
    {
        CAPTURE(text);
        CHECK_FALSE(parse(text, *scenario).is_ok());
    }
}

TEST_CASE("CW fading scenario: combines with fixed, sweep, FSK and either gate", "[cw][fading][scenario]")
{
    auto scenario = std::make_unique<sl::Scenario>();
    REQUIRE(parse(cw_scenario(R"(,"fading":{"spread_hz":1},"fsk":{"baud":300,"shift_hz":300,"pattern":"alternating"},"pulse_period_seconds":0.01,"pulse_duty_cycle":0.5)"), *scenario).is_ok());
    CHECK(scenario->stages[0].cw.fading.enabled);
    CHECK(scenario->stages[0].cw.fsk.enabled);
    CHECK(scenario->stages[0].cw.pulse_period_frames == 480U);
    CHECK(sl::validate_scenario(*scenario).is_ok());
    constexpr std::string_view swept =
        R"({"impairments":[{"type":"cw","frequency_sweep":{"low_hz":11000,"high_hz":13000,"slope_hz_per_second":2000},"ci_db":0,"fading":{"spread_hz":10},"markov_on_off":{"mean_on_seconds":0.005,"mean_off_seconds":0.008}}]})";
    REQUIRE(parse(swept, *scenario).is_ok());
    CHECK(scenario->stages[0].cw.sweep.enabled);
    CHECK(scenario->stages[0].cw.markov.enabled);
    CHECK(sl::validate_scenario(*scenario).is_ok());
}

TEST_CASE("CW fading scenario: typed descriptors validate enabled spread", "[cw][fading][scenario]")
{
    auto scenario = std::make_unique<sl::Scenario>();
    REQUIRE(parse(cw_scenario(R"(,"fading":{"spread_hz":1})"), *scenario).is_ok());
    scenario->stages[0].cw.fading.spread = sl::units::FrequencyDelta::from_hz(0.0);
    CHECK_FALSE(sl::validate_scenario(*scenario).is_ok());
    scenario->stages[0].cw.fading.spread = sl::units::FrequencyDelta::from_hz(10.1);
    CHECK_FALSE(sl::validate_scenario(*scenario).is_ok());
    scenario->stages[0].cw.fading.spread = sl::units::FrequencyDelta::from_hz(10.0);
    CHECK(sl::validate_scenario(*scenario).is_ok());
}
