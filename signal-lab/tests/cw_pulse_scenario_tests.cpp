// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// CW pulse schedule scenario grammar and typed-descriptor validation.

#include "signal_lab/scenario.hpp"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
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

constexpr std::string_view cw_prefix = R"({"impairments":[{"type":"cw","frequency_hz":1000,"ci_db":0)";

[[nodiscard]] std::string full_scenario(std::string_view cw_members)
{
    return std::string{cw_prefix} + std::string{cw_members} + R"(}]})";
}

} // namespace

TEST_CASE("CW pulse scenario: defaults, exact frame conversion, and maximum", "[cw][pulse][scenario]")
{
    auto scenario = std::make_unique<sl::Scenario>();
    REQUIRE(parse(full_scenario(""), *scenario).is_ok());
    CHECK(scenario->stages[0].cw.pulse_period_frames == 0U);
    CHECK(scenario->stages[0].cw.pulse_on_frames == 0U);
    REQUIRE(parse(full_scenario(R"(,"pulse_period_seconds":1,"pulse_duty_cycle":0.25)"), *scenario).is_ok());
    CHECK(scenario->stages[0].cw.pulse_period_frames == 48000U);
    CHECK(scenario->stages[0].cw.pulse_on_frames == 12000U);
    REQUIRE(parse(full_scenario(R"(,"pulse_period_seconds":0.00008333333333333333,"pulse_duty_cycle":0.25)"), *scenario).is_ok());
    CHECK(scenario->stages[0].cw.pulse_period_frames == 4U);
    CHECK(scenario->stages[0].cw.pulse_on_frames == 1U);
    REQUIRE(parse(full_scenario(R"(,"pulse_period_seconds":0.00008333333333333333,"pulse_duty_cycle":1)"), *scenario).is_ok());
    CHECK(scenario->stages[0].cw.pulse_period_frames == 4U);
    CHECK(scenario->stages[0].cw.pulse_on_frames == 4U);
    REQUIRE(parse(full_scenario(R"(,"pulse_period_seconds":1000000000,"pulse_duty_cycle":1)"), *scenario).is_ok());
    CHECK(scenario->stages[0].cw.pulse_period_frames == sl::max_cw_pulse_frames);
    CHECK(scenario->stages[0].cw.pulse_on_frames == sl::max_cw_pulse_frames);
}

TEST_CASE("CW pulse scenario: paired fields and exact numeric frames are strict", "[cw][pulse][scenario]")
{
    constexpr std::array refused{
        R"({"impairments":[{"type":"cw","frequency_hz":1000,"ci_db":0,"pulse_period_seconds":1}]})",
        R"({"impairments":[{"type":"cw","frequency_hz":1000,"ci_db":0,"pulse_duty_cycle":0.5}]})",
        R"({"impairments":[{"type":"cw","frequency_hz":1000,"ci_db":0,"pulse_period_seconds":true,"pulse_duty_cycle":0.5}]})",
        R"({"impairments":[{"type":"cw","frequency_hz":1000,"ci_db":0,"pulse_period_seconds":null,"pulse_duty_cycle":0.5}]})",
        R"({"impairments":[{"type":"cw","frequency_hz":1000,"ci_db":0,"pulse_period_seconds":0,"pulse_duty_cycle":0.5}]})",
        R"({"impairments":[{"type":"cw","frequency_hz":1000,"ci_db":0,"pulse_period_seconds":-1,"pulse_duty_cycle":0.5}]})",
        R"({"impairments":[{"type":"cw","frequency_hz":1000,"ci_db":0,"pulse_period_seconds":0.0001,"pulse_duty_cycle":0.5}]})",
        R"({"impairments":[{"type":"cw","frequency_hz":1000,"ci_db":0,"pulse_period_seconds":1000000001,"pulse_duty_cycle":1}]})",
        R"({"impairments":[{"type":"cw","frequency_hz":1000,"ci_db":0,"pulse_period_seconds":1,"pulse_duty_cycle":false}]})",
        R"({"impairments":[{"type":"cw","frequency_hz":1000,"ci_db":0,"pulse_period_seconds":1,"pulse_duty_cycle":null}]})",
        R"({"impairments":[{"type":"cw","frequency_hz":1000,"ci_db":0,"pulse_period_seconds":1,"pulse_duty_cycle":0}]})",
        R"({"impairments":[{"type":"cw","frequency_hz":1000,"ci_db":0,"pulse_period_seconds":1,"pulse_duty_cycle":1.1}]})",
        R"({"impairments":[{"type":"cw","frequency_hz":1000,"ci_db":0,"pulse_period_seconds":0.00008333333333333333,"pulse_duty_cycle":0.3}]})",
        R"({"impairments":[{"type":"cw","frequency_hz":1000,"ci_db":0,"pulse_period_seconds":0.00008333333333333333,"pulse_duty_cycle":0.000001}]})",
        R"({"impairments":[{"type":"cw","frequency_hz":1000,"ci_db":0,"pulse_period_seconds":1,"pulse_period_seconds":2,"pulse_duty_cycle":0.5}]})",
        R"({"impairments":[{"type":"cw","frequency_hz":1000,"ci_db":0,"pulse_period_seconds":1,"pulse_duty_cycle":0.5,"pulse_duty_cycle":0.25}]})",
    };
    auto scenario = std::make_unique<sl::Scenario>();

    for (const std::string_view text : refused)
    {
        CAPTURE(text);
        CHECK_FALSE(parse(text, *scenario).is_ok());
    }
}

TEST_CASE("CW pulse scenario: unknown CW members retain legacy handling", "[cw][pulse][scenario]")
{
    auto scenario = std::make_unique<sl::Scenario>();
    REQUIRE(parse(full_scenario(R"(,"future_cw_field":7)"), *scenario).is_ok());
    CHECK(scenario->stages[0].cw.pulse_period_frames == 0U);
    CHECK(scenario->stages[0].cw.pulse_on_frames == 0U);
}

TEST_CASE("CW pulse scenario: programmatic timing must be disabled or bounded", "[cw][pulse][scenario]")
{
    auto scenario = std::make_unique<sl::Scenario>();
    REQUIRE(parse(full_scenario(""), *scenario).is_ok());
    auto& params = scenario->stages[0].cw;
    CHECK(sl::validate_scenario(*scenario).is_ok());
    params.pulse_period_frames = 1U;
    CHECK_FALSE(sl::validate_scenario(*scenario).is_ok());
    params.pulse_on_frames = 1U;
    CHECK(sl::validate_scenario(*scenario).is_ok());
    params.pulse_on_frames = 2U;
    CHECK_FALSE(sl::validate_scenario(*scenario).is_ok());
    params.pulse_period_frames = sl::max_cw_pulse_frames + 1U;
    params.pulse_on_frames = 1U;
    CHECK_FALSE(sl::validate_scenario(*scenario).is_ok());
    params.pulse_period_frames = 0U;
    params.pulse_on_frames = 0U;
    CHECK(sl::validate_scenario(*scenario).is_ok());
}
