// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// CW fixed/swept frequency scenario grammar and validation.

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
    return std::string{R"({"impairments":[{"type":"cw")"} + std::string{fields} + R"(}]})";
}

} // namespace

TEST_CASE("CW sweep scenario: legacy fixed frequency remains the default form", "[cw][sweep][scenario]")
{
    auto scenario = std::make_unique<sl::Scenario>();
    REQUIRE(parse(cw_scenario(R"(,"frequency_hz":1200,"ci_db":-3)"), *scenario).is_ok());
    CHECK(scenario->stages[0].cw.frequency_hz == 1200.0);
    CHECK_FALSE(scenario->stages[0].cw.sweep.enabled);
    CHECK(scenario->stages[0].cw.sweep.low.hertz() == 0.0);
    CHECK(scenario->stages[0].cw.pulse_period_frames == 0U);
    CHECK(scenario->stages[0].cw.pulse_on_frames == 0U);
}

TEST_CASE("CW sweep scenario: strict triangle grammar and frame-aligned pulse can combine", "[cw][sweep][scenario]")
{
    auto scenario = std::make_unique<sl::Scenario>();
    REQUIRE(parse(cw_scenario(R"(,"frequency_sweep":{"low_hz":1000,"high_hz":2000,"slope_hz_per_second":10},"ci_db":0,"pulse_period_seconds":1,"pulse_duty_cycle":0.25)"), *scenario).is_ok());
    const sl::CwParams& cw = scenario->stages[0].cw;
    CHECK(cw.frequency_hz == 0.0);
    CHECK(cw.sweep.enabled);
    CHECK(cw.sweep.low.hertz() == 1000.0);
    CHECK(cw.sweep.high.hertz() == 2000.0);
    CHECK(cw.sweep.slope.hertz_per_second() == 10.0);
    CHECK(cw.pulse_period_frames == 48000U);
    CHECK(cw.pulse_on_frames == 12000U);
}

TEST_CASE("CW sweep scenario: low and high frequency edges are inclusive", "[cw][sweep][scenario]")
{
    auto scenario = std::make_unique<sl::Scenario>();
    REQUIRE(parse(cw_scenario(R"(,"frequency_sweep":{"low_hz":1,"high_hz":23999,"slope_hz_per_second":1},"ci_db":0)"), *scenario).is_ok());
    CHECK(scenario->stages[0].cw.sweep.low.hertz() == 1.0);
    CHECK(scenario->stages[0].cw.sweep.high.hertz() == 23999.0);
}

TEST_CASE("CW sweep scenario: fixed or sweep frequency selection is required exactly once", "[cw][sweep][scenario]")
{
    constexpr std::array refused{
        R"({"impairments":[{"type":"cw","ci_db":0}]})",
        R"({"impairments":[{"type":"cw","frequency_hz":1000,"frequency_sweep":{"low_hz":1000,"high_hz":2000,"slope_hz_per_second":10},"ci_db":0}]})",
        R"({"impairments":[{"type":"cw","frequency_hz":1000,"frequency_hz":1200,"ci_db":0}]})",
        R"({"impairments":[{"type":"cw","frequency_sweep":{"low_hz":1000,)"
        R"("high_hz":2000,"slope_hz_per_second":10},"frequency_sweep":{"low_hz":1100,)"
        R"("high_hz":2100,"slope_hz_per_second":10},"ci_db":0}]})",
    };
    auto scenario = std::make_unique<sl::Scenario>();

    for (const std::string_view text : refused)
    {
        CAPTURE(text);
        CHECK_FALSE(parse(text, *scenario).is_ok());
    }
}

TEST_CASE("CW sweep scenario: nested object requires exactly three unique numeric members", "[cw][sweep][scenario]")
{
    constexpr std::array refused{
        R"({"impairments":[{"type":"cw","frequency_sweep":null,"ci_db":0}]})",
        R"({"impairments":[{"type":"cw","frequency_sweep":{"high_hz":2000,"slope_hz_per_second":10},"ci_db":0}]})",
        R"({"impairments":[{"type":"cw","frequency_sweep":{"low_hz":1000,"slope_hz_per_second":10},"ci_db":0}]})",
        R"({"impairments":[{"type":"cw","frequency_sweep":{"low_hz":1000,"high_hz":2000},"ci_db":0}]})",
        R"({"impairments":[{"type":"cw","frequency_sweep":{"low_hz":1000,"high_hz":2000,"slope_hz_per_second":10,"future":1},"ci_db":0}]})",
        R"({"impairments":[{"type":"cw","frequency_sweep":{"low_hz":1000,"low_hz":1100,"high_hz":2000,"slope_hz_per_second":10},"ci_db":0}]})",
        R"({"impairments":[{"type":"cw","frequency_sweep":{"low_hz":1000,"high_hz":2000,"high_hz":2100,"slope_hz_per_second":10},"ci_db":0}]})",
        R"({"impairments":[{"type":"cw","frequency_sweep":{"low_hz":1000,)"
        R"("high_hz":2000,"slope_hz_per_second":10,"slope_hz_per_second":11},"ci_db":0}]})",
        R"({"impairments":[{"type":"cw","frequency_sweep":{"low_hz":true,"high_hz":2000,"slope_hz_per_second":10},"ci_db":0}]})",
        R"({"impairments":[{"type":"cw","frequency_sweep":{"low_hz":1000,"high_hz":null,"slope_hz_per_second":10},"ci_db":0}]})",
        R"({"impairments":[{"type":"cw","frequency_sweep":{"low_hz":1000,"high_hz":2000,"slope_hz_per_second":"10"},"ci_db":0}]})",
    };
    auto scenario = std::make_unique<sl::Scenario>();

    for (const std::string_view text : refused)
    {
        CAPTURE(text);
        CHECK_FALSE(parse(text, *scenario).is_ok());
    }
}

TEST_CASE("CW sweep scenario: invalid endpoints and unrepresentable slopes refuse", "[cw][sweep][scenario]")
{
    constexpr std::array refused{
        R"({"impairments":[{"type":"cw","frequency_sweep":{"low_hz":0,"high_hz":2000,"slope_hz_per_second":10},"ci_db":0}]})",
        R"({"impairments":[{"type":"cw","frequency_sweep":{"low_hz":1000,"high_hz":24000,"slope_hz_per_second":10},"ci_db":0}]})",
        R"({"impairments":[{"type":"cw","frequency_sweep":{"low_hz":2000,"high_hz":1000,"slope_hz_per_second":10},"ci_db":0}]})",
        R"({"impairments":[{"type":"cw","frequency_sweep":{"low_hz":1000,"high_hz":2000,"slope_hz_per_second":0},"ci_db":0}]})",
        R"({"impairments":[{"type":"cw","frequency_sweep":{"low_hz":1000,"high_hz":2000,"slope_hz_per_second":-1},"ci_db":0}]})",
        R"({"impairments":[{"type":"cw","frequency_sweep":{"low_hz":1000,"high_hz":2000,"slope_hz_per_second":1e-300},"ci_db":0}]})",
        R"({"impairments":[{"type":"cw","frequency_sweep":{"low_hz":1000,"high_hz":2000,"slope_hz_per_second":1e308},"ci_db":0}]})",
    };
    auto scenario = std::make_unique<sl::Scenario>();

    for (const std::string_view text : refused)
    {
        CAPTURE(text);
        CHECK_FALSE(parse(text, *scenario).is_ok());
    }
}

TEST_CASE("CW sweep scenario: typed descriptor refuses simultaneous fixed and swept frequency", "[cw][sweep][scenario]")
{
    auto scenario = std::make_unique<sl::Scenario>();
    REQUIRE(parse(cw_scenario(R"(,"frequency_sweep":{"low_hz":1000,"high_hz":2000,"slope_hz_per_second":10},"ci_db":0)"), *scenario).is_ok());
    scenario->stages[0].cw.frequency_hz = 1200.0;
    CHECK_FALSE(sl::validate_scenario(*scenario).is_ok());
}
