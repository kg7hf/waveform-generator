// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// Catch2 suite of the Watterson stage's scenario schema (design 9.3): test U25, the
// stage parser parse_watterson_stage() and the programmatic validate_watterson().
// Guide: docs/modules/signal-lab-watterson.md, "Scenario schema".

#include "signal-lab/src/watterson_scenario.hpp"
#include "signal_lab/json.hpp"
#include "signal_lab/watterson.hpp"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <string_view>

namespace
{

namespace sl = signal_lab;
namespace units = signal_lab::units;

// The schema's messages, as the design lists them (design 9.3).
constexpr std::string_view paths_message = "watterson.paths: required, 1 to 4 path objects";
constexpr std::string_view stage_member_message = "watterson: unknown member";
constexpr std::string_view path_member_message = "watterson.paths[]: unknown member";
constexpr std::string_view delay_message = "watterson.paths[].delay_ms: number in [0, 20]";
constexpr std::string_view power_message = "watterson.paths[].power_db: number in [-60, 60]";
constexpr std::string_view fading_message = R"(watterson.paths[].fading: "rayleigh" or "fixed")";
constexpr std::string_view spread_message = "watterson.paths[].spread_hz: required for a rayleigh path, number in [0.1, 10]";
constexpr std::string_view spread_fixed_message = "watterson.paths[].spread_hz: not allowed on a fixed path";
constexpr std::string_view shift_message = "watterson.paths[].shift_hz: number in [-75, 75]";
constexpr std::string_view phase_message = "watterson.paths[].phase_deg: number in [-360, 360], fixed paths only";
constexpr std::string_view finite_message = "watterson.paths[]: value is not finite";
constexpr std::string_view stage_duplicate_message = "watterson: duplicate member";
constexpr std::string_view path_duplicate_message = "watterson.paths[]: duplicate member";

struct RefusalCase
{
    std::string_view json;
    std::string_view message;
};

constexpr std::array refusals{
    RefusalCase{R"({"type": "watterson"})", paths_message},
    RefusalCase{R"({"type": "watterson", "paths": []})", paths_message},
    RefusalCase{R"({"type": "watterson", "paths": {"spread_hz": 1}})", paths_message},
    RefusalCase{R"({"type": "watterson", "paths": [1]})", paths_message},
    RefusalCase{R"({"type": "watterson", "paths": [{"spread_hz": 1}, {"spread_hz": 1}, {"spread_hz": 1}, {"spread_hz": 1}, {"spread_hz": 1}]})", paths_message},
    RefusalCase{R"({"type": "watterson", "paths": [{"spread_hz": 1}], "seed": 3})", stage_member_message},
    RefusalCase{R"({"type": "watterson", "paths": [{"spread_hz": 1, "shift": 3}]})", path_member_message},
    RefusalCase{R"({"type": "watterson", "paths": [{"spread_hz": 1, "delay_ms": 20.5}]})", delay_message},
    RefusalCase{R"({"type": "watterson", "paths": [{"spread_hz": 1, "delay_ms": -1}]})", delay_message},
    RefusalCase{R"({"type": "watterson", "paths": [{"spread_hz": 1, "delay_ms": "2"}]})", delay_message},
    RefusalCase{R"({"type": "watterson", "paths": [{"spread_hz": 1, "power_db": 61}]})", power_message},
    RefusalCase{R"({"type": "watterson", "paths": [{"fading": "rician"}]})", fading_message},
    RefusalCase{R"({"type": "watterson", "paths": [{"fading": 1}]})", fading_message},
    RefusalCase{R"({"type": "watterson", "paths": [{"delay_ms": 2}]})", spread_message},
    RefusalCase{R"({"type": "watterson", "paths": [{"spread_hz": 0.05}]})", spread_message},
    RefusalCase{R"({"type": "watterson", "paths": [{"spread_hz": 10.5}]})", spread_message},
    RefusalCase{R"({"type": "watterson", "paths": [{"fading": "fixed", "spread_hz": 1}]})", spread_fixed_message},
    RefusalCase{R"({"type": "watterson", "paths": [{"spread_hz": 1, "shift_hz": 75.5}]})", shift_message},
    RefusalCase{R"({"type": "watterson", "paths": [{"spread_hz": 1, "phase_deg": 10}]})", phase_message},
    RefusalCase{R"({"type": "watterson", "paths": [{"fading": "fixed", "phase_deg": 361}]})", phase_message},
    // A member named twice: one reader would take the first, another the last.
    RefusalCase{R"({"type": "watterson", "paths": [{"spread_hz": 1}], "paths": [{"spread_hz": 5}]})", stage_duplicate_message},
    RefusalCase{R"({"type": "watterson", "type": "watterson", "paths": [{"spread_hz": 1}]})", stage_duplicate_message},
    RefusalCase{R"({"type": "watterson", "paths": [{"spread_hz": 1, "delay_ms": 2, "delay_ms": 7}]})", path_duplicate_message},
    RefusalCase{R"({"type": "watterson", "paths": [{"spread_hz": 1, "spread_hz": 1}]})", path_duplicate_message},
    RefusalCase{R"({"type": "watterson", "paths": [{"fading": "fixed", "fading": "rayleigh", "spread_hz": 1}]})", path_duplicate_message},
    RefusalCase{R"({"type": "watterson", "paths": [{"fading": "fixed", "phase_deg": 10, "phase_deg": 20}]})", path_duplicate_message},
    RefusalCase{R"({"type": "watterson", "paths": [{"spread_hz": 1, "shift_hz": 3, "power_db": 0, "shift_hz": 3}]})", path_duplicate_message},
};

// CCIR / ITU-R Poor (D.6.1, F.1487): two equal Rayleigh paths, 2 ms, 1 Hz.
constexpr std::string_view ccir_poor = R"({"type": "watterson", "paths": [{"delay_ms": 0.0, "spread_hz": 1.0}, {"delay_ms": 2.0, "spread_hz": 1.0}]})";
constexpr double poor_delay_ms = 2.0;
constexpr double poor_spread_hz = 1.0;
// D.6.2, 3 kHz WID 10: three fixed paths at 0, 2 and 4.5 ms.
constexpr std::string_view wid_ten = R"({"type": "watterson", "paths": [{"fading": "fixed"}, {"delay_ms": 2.0, "fading": "fixed"}, {"delay_ms": 4.5, "fading": "fixed"}]})";
constexpr double wid_ten_last_ms = 4.5;
// Every member of a path, at a range edge.
constexpr std::string_view every_member =
    R"({"type": "watterson", "paths": [{"delay_ms": 20, "power_db": -60, "fading": "fixed", "shift_hz": -75, "phase_deg": -360}, {"spread_hz": 10, "power_db": 60, "shift_hz": 75}]})";
constexpr double edge_delay_ms = 20.0;
constexpr double edge_level_db = 60.0;
constexpr double edge_shift_hz = 75.0;
constexpr double edge_phase_degrees = 360.0;
constexpr double edge_spread_hz = 10.0;

struct Parsed
{
    sl::WattersonParams params{};
    m110::Status status{};
};

[[nodiscard]] Parsed parse(std::string_view text)
{
    auto document = std::make_unique<sl::json::Document>();
    Parsed parsed{};

    if (!document->parse(text.data(), text.size()).is_ok() || document->root() == nullptr)
    {
        parsed.status = m110::Status{m110::StatusCode::invalid_argument, "test JSON does not parse"};
        return parsed;
    }

    parsed.status = sl::parse_watterson_stage(*document, *document->root(), parsed.params);
    return parsed;
}

} // namespace

TEST_CASE("watterson schema: CCIR Poor parses with the defaults applied (U25)", "[watterson][schema]")
{
    const Parsed parsed = parse(ccir_poor);
    REQUIRE(parsed.status.is_ok());
    CHECK(parsed.params.path_count == 2U);
    const sl::WattersonPath& second = parsed.params.paths[1];
    CHECK(second.delay == units::Duration{poor_delay_ms, units::ms});
    CHECK(second.spread == units::FrequencyDelta{poor_spread_hz, units::Hz});
    CHECK(second.fading == sl::PathFading::rayleigh);
    CHECK(second.level == units::Gain{});
    CHECK(second.shift == units::FrequencyDelta{});
    CHECK(second.phase == units::Angle{});
}

TEST_CASE("watterson schema: D.6.2 fixed paths and every member at its range edge parse (U25)", "[watterson][schema]")
{
    const Parsed fixed = parse(wid_ten);
    REQUIRE(fixed.status.is_ok());
    CHECK(fixed.params.path_count == 3U);
    CHECK(fixed.params.paths[2].fading == sl::PathFading::fixed);
    CHECK(fixed.params.paths[2].delay == units::Duration{wid_ten_last_ms, units::ms});
    const Parsed edges = parse(every_member);
    REQUIRE(edges.status.is_ok());
    const sl::WattersonPath& first = edges.params.paths[0];
    const sl::WattersonPath& second = edges.params.paths[1];
    CHECK(first.delay == units::Duration{edge_delay_ms, units::ms});
    CHECK(first.level == units::Gain{-edge_level_db, units::dB});
    CHECK(first.shift == units::FrequencyDelta{-edge_shift_hz, units::Hz});
    CHECK(first.phase == units::Angle{-edge_phase_degrees, units::degrees});
    CHECK(second.spread == units::FrequencyDelta{edge_spread_hz, units::Hz});
    CHECK(second.level == units::Gain{edge_level_db, units::dB});
    CHECK(second.shift == units::FrequencyDelta{edge_shift_hz, units::Hz});
}

TEST_CASE("watterson schema: every refusal names its member and range (U25)", "[watterson][schema]")
{
    for (const RefusalCase& refusal : refusals)
    {
        const Parsed parsed = parse(refusal.json);
        INFO(refusal.json);
        CHECK_FALSE(parsed.status.is_ok());
        CHECK(std::string_view{parsed.status.message} == refusal.message);
    }
}

TEST_CASE("watterson schema: programmatic parameters are validated the same way (U25)", "[watterson][schema]")
{
    sl::WattersonParams params{};
    CHECK(std::string_view{sl::validate_watterson(params).message} == paths_message);
    params.path_count = 1U;
    params.paths[0].spread = units::FrequencyDelta{poor_spread_hz, units::Hz};
    CHECK(sl::validate_watterson(params).is_ok());
    params.paths[0].delay = units::Duration{std::numeric_limits<double>::quiet_NaN(), units::ms};
    CHECK(std::string_view{sl::validate_watterson(params).message} == finite_message);
    params.paths[0].delay = units::Duration{};
    params.paths[0].shift = units::FrequencyDelta{std::numeric_limits<double>::infinity(), units::Hz};
    CHECK(std::string_view{sl::validate_watterson(params).message} == finite_message);
    params.paths[0].shift = units::FrequencyDelta{};
    params.paths[0].fading = sl::PathFading::fixed;
    CHECK(std::string_view{sl::validate_watterson(params).message} == spread_fixed_message);
    params.path_count = sl::watterson_max_paths + 1U;
    CHECK(std::string_view{sl::validate_watterson(params).message} == paths_message);
}

TEST_CASE("watterson schema: a refused configuration leaves the channel unconfigured", "[watterson][schema]")
{
    auto channel = std::make_unique<sl::WattersonChannel>();
    const sl::WattersonParams empty{};
    const m110::Status status = channel->configure(empty, 1U, 0U, nullptr);
    CHECK(std::string_view{status.message} == paths_message);
    constexpr std::size_t frames = 64U;
    std::array<float, frames> block{};
    CHECK_FALSE(channel->process(block, units::SampleIndex{}).is_ok());
}
