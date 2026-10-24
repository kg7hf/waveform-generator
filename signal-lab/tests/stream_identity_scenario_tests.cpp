// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// Per-stage random-stream identity grammar and validation.

#include "signal_lab/scenario.hpp"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <string_view>

namespace
{

namespace sl = signal_lab;

[[nodiscard]] m110::Status parse(std::string_view text, sl::Scenario& scenario)
{
    return sl::parse_scenario(text.data(), text.size(), scenario);
}

} // namespace

TEST_CASE("stream identity: omitted IDs remain zero and boundaries parse", "[scenario][stream-id]")
{
    auto scenario = std::make_unique<sl::Scenario>();
    REQUIRE(parse(R"({"impairments":[{"type":"awgn","snr_db":0},{"type":"cw","frequency_hz":12000,"ci_db":0,"stream_id":4294967295},{"type":"awgn","snr_db":1,"stream_id":1}]})", *scenario).is_ok());
    REQUIRE(scenario->stage_count == 3U);
    CHECK(scenario->stages[0].stream_id == 0U);
    CHECK(scenario->stages[1].stream_id == UINT32_MAX);
    CHECK(scenario->stages[2].stream_id == 1U);
    CHECK(sl::validate_scenario(*scenario).is_ok());
}

TEST_CASE("stream identity: every stage type accepts its own ID", "[scenario][stream-id]")
{
    constexpr std::string_view text = R"({"impairments":[
        {"type":"watterson","stream_id":101,"paths":[{"fading":"rayleigh","spread_hz":1}]},
        {"type":"frequency_shift","mode":"offset","offset_hz":75,"stream_id":102},
        {"type":"awgn","snr_db":3,"stream_id":103},
        {"type":"cw","frequency_hz":12000,"ci_db":12,"stream_id":104},
        {"type":"impulse","peak_db":-12,"decay_ms":8,"ring":"noise","rate_per_sec":100,"stream_id":105},
        {"type":"fade","depth_db":12,"duration_ms":5,"period_seconds":0.05,"stream_id":106},
        {"type":"sample_slip","kind":"delete","length_samples":1,"at_seconds":0.1,"stream_id":107}
    ]})";
    auto scenario = std::make_unique<sl::Scenario>();
    REQUIRE(parse(text, *scenario).is_ok());
    REQUIRE(scenario->stage_count == 7U);

    for (std::uint32_t index = 0U; index < scenario->stage_count; ++index)
    {
        CHECK(scenario->stages[index].stream_id == index + 101U);
    }

    CHECK(sl::validate_scenario(*scenario).is_ok());
}

TEST_CASE("stream identity: malformed values and duplicate members are refused", "[scenario][stream-id]")
{
    constexpr std::array refused{
        R"({"impairments":[{"type":"awgn","snr_db":0,"stream_id":0}]})",
        R"({"impairments":[{"type":"awgn","snr_db":0,"stream_id":true}]})",
        R"({"impairments":[{"type":"awgn","snr_db":0,"stream_id":1.5}]})",
        R"({"impairments":[{"type":"awgn","snr_db":0,"stream_id":-1}]})",
        R"({"impairments":[{"type":"awgn","snr_db":0,"stream_id":4294967296}]})",
        R"({"impairments":[{"type":"awgn","snr_db":0,"stream_id":1e309}]})",
        R"({"impairments":[{"type":"awgn","snr_db":0,"stream_id":"1"}]})",
        R"({"impairments":[{"type":"awgn","snr_db":0,"stream_id":null}]})",
        R"({"impairments":[{"type":"awgn","snr_db":0,"stream_id":1,"stream_id":2}]})",
    };
    auto scenario = std::make_unique<sl::Scenario>();

    for (const std::string_view text : refused)
    {
        CAPTURE(text);
        CHECK_FALSE(parse(text, *scenario).is_ok());
    }
}

TEST_CASE("stream identity: duplicate values across different stage types are refused", "[scenario][stream-id]")
{
    auto scenario = std::make_unique<sl::Scenario>();
    constexpr std::string_view text = R"({"impairments":[{"type":"awgn","snr_db":0,"stream_id":42},{"type":"cw","frequency_hz":12000,"ci_db":0,"stream_id":42}]})";
    CHECK_FALSE(parse(text, *scenario).is_ok());
    constexpr std::string_view deterministic_stages =
        R"({"impairments":[{"type":"frequency_shift","mode":"offset","offset_hz":5,"stream_id":42},{"type":"cw","frequency_hz":12000,"ci_db":0,"stream_id":42}]})";
    CHECK_FALSE(parse(deterministic_stages, *scenario).is_ok());
}

TEST_CASE("stream identity: typed descriptors enforce nonzero uniqueness", "[scenario][stream-id]")
{
    auto scenario = std::make_unique<sl::Scenario>();
    REQUIRE(parse(R"({"impairments":[{"type":"awgn","snr_db":0,"stream_id":10},{"type":"cw","frequency_hz":12000,"ci_db":0,"stream_id":11}]})", *scenario).is_ok());
    scenario->stages[1].stream_id = scenario->stages[0].stream_id;
    CHECK_FALSE(sl::validate_scenario(*scenario).is_ok());
    scenario->stages[0].stream_id = 0U;
    scenario->stages[1].stream_id = 0U;
    CHECK(sl::validate_scenario(*scenario).is_ok());
}
