// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// Catch2 suite of the Watterson stage inside the engine: registration (StageType,
// the scenario slot, the dispatchers, the typed slot and StageResources), tests U26
// to U28 and U36 of the design, and the algorithmic x-to-y delay of E.4.4.
// Guide: docs/modules/signal-lab-watterson.md, "In the engine".

#include "signal-lab/src/watterson_scenario.hpp"
#include "signal_lab/analytic_filter.hpp"
#include "signal_lab/convert.hpp"
#include "signal_lab/det_math.hpp"
#include "signal_lab/impairment.hpp"
#include "signal_lab/json.hpp"
#include "signal_lab/mixer.hpp"
#include "signal_lab/scenario.hpp"
#include "signal_lab/watterson.hpp"
#include "test_convert.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace
{

namespace sl = signal_lab;
namespace units = signal_lab::units;

constexpr std::string_view single_message = "watterson: at most one stage per scenario";
constexpr std::string_view first_message = "watterson: must be the first stage (noise and interference follow the channel, E.4.1)";
constexpr std::string_view channel_message = "watterson: needs a WattersonChannel (StageResources.watterson)";
constexpr std::string_view unconfigured_message = "watterson: process() before a successful configure()";
constexpr std::string_view contiguity_message = "watterson: first_input_frame does not follow the previous block";
constexpr std::string_view watterson_name = "watterson";

constexpr std::string_view ccir_poor = R"({"seed": 5, "source_gain_db": 0, "impairments": [{"type": "watterson", "paths": [{"spread_hz": 1}, {"delay_ms": 2, "spread_hz": 1}]}]})";
constexpr std::string_view after_noise = R"({"impairments": [{"type": "awgn", "snr_db": 20}, {"type": "watterson", "paths": [{"spread_hz": 1}]}]})";
constexpr std::string_view twice = R"({"impairments": [{"type": "watterson", "paths": [{"spread_hz": 1}]}, {"type": "watterson", "paths": [{"spread_hz": 2}]}]})";
constexpr std::string_view then_noise = R"({"impairments": [{"type": "watterson", "paths": [{"spread_hz": 1}]}, {"type": "awgn", "snr_db": 20}]})";
constexpr std::string_view with_legacy = R"({"impairments": [{"type": "watterson", "paths": [{"spread_hz": 1}, {"delay_ms": 2, "spread_hz": 1}]}, {"type": "cw", "frequency_hz": 1800, "ci_db": 6},)"
                                         R"( {"type": "impulse", "rate_per_sec": 1.0, "peak_db": 20, "decay_ms": 20}]})";
constexpr std::string_view legacy_only = R"({"impairments": [{"type": "cw", "frequency_hz": 1800, "ci_db": 6}, {"type": "impulse", "rate_per_sec": 1.0, "peak_db": 20, "decay_ms": 20}]})";
// E.4.4 (algorithmic): one fixed path at 2 ms, unity source gain.
constexpr std::string_view fixed_two_ms = R"({"source_gain_db": 0, "impairments": [{"type": "watterson", "paths": [{"delay_ms": 2.0, "fading": "fixed"}]}]})";
constexpr std::uint64_t two_ms_frames = 96U;

// U36: the stage-level refusals of U25, which the full parser must report the same way.
constexpr std::array stage_refusals{
    std::string_view{R"({"type": "watterson"})"},
    std::string_view{R"({"type": "watterson", "paths": []})"},
    std::string_view{R"({"type": "watterson", "paths": [{"spread_hz": 1}], "seed": 3})"},
    std::string_view{R"({"type": "watterson", "paths": [{"spread_hz": 1, "shift": 3}]})"},
    std::string_view{R"({"type": "watterson", "paths": [{"spread_hz": 1, "delay_ms": 20.5}]})"},
    std::string_view{R"({"type": "watterson", "paths": [{"spread_hz": 1, "power_db": 61}]})"},
    std::string_view{R"({"type": "watterson", "paths": [{"fading": "rician"}]})"},
    std::string_view{R"({"type": "watterson", "paths": [{"spread_hz": 0.05}]})"},
    std::string_view{R"({"type": "watterson", "paths": [{"fading": "fixed", "spread_hz": 1}]})"},
    std::string_view{R"({"type": "watterson", "paths": [{"spread_hz": 1, "shift_hz": 75.5}]})"},
    std::string_view{R"({"type": "watterson", "paths": [{"spread_hz": 1, "phase_deg": 10}]})"},
};

constexpr double reference_rms = 0.125;
constexpr std::uint64_t total_frames = 480000U;
constexpr std::size_t engine_block = 2048U;
constexpr std::size_t render_frames = 30000U;
constexpr std::array latency_blocks{std::size_t{1}, std::size_t{96}, std::size_t{448}, std::size_t{2048}};
constexpr std::size_t impulse_frame = 5000U;
constexpr std::size_t impulse_frames = 6000U;
constexpr std::uint64_t ccir_seed = 5U;
constexpr double input_centre = 0.5;
// A small input, so no fading peak reaches the engine's clip and both paths compare.
constexpr double input_scale = 0.05;
// The impulse stays inside PCM24 full scale (the engine saturates at 1 - 2^-23).
constexpr float impulse_height = 0.5F;
constexpr std::uint64_t input_seed = 21U;

struct Configured
{
    std::unique_ptr<sl::Scenario> scenario = std::make_unique<sl::Scenario>();
    std::unique_ptr<sl::Engine> engine = std::make_unique<sl::Engine>();
    m110::Status status{};
    bool parsed{};
    bool ok{};
};

[[nodiscard]] Configured configure(std::string_view text, const sl::StageResources& resources)
{
    Configured result;
    result.status = sl::parse_scenario(text.data(), text.size(), *result.scenario);
    result.parsed = result.status.is_ok();

    if (result.parsed)
    {
        result.status = result.engine->configure(*result.scenario, reference_rms, total_frames, resources);
    }

    result.ok = result.status.is_ok();
    return result;
}

[[nodiscard]] sl::WattersonChannel& shared_channel()
{
    constinit static sl::WattersonChannel channel{};
    return channel;
}

[[nodiscard]] std::vector<float> render(sl::Engine& engine, std::span<const float> input, std::size_t block, bool& ok)
{
    std::vector<float> output;
    std::vector<float> buffer(sl::engine_capacity_frames);
    ok = true;

    for (std::size_t offset = 0U; offset < input.size(); offset += block)
    {
        const std::size_t count = std::min(block, input.size() - offset);
        const m110::Result<std::size_t> produced = engine.process(input.subspan(offset, count), buffer);
        ok = ok && produced.has_value() && produced.value() == count;
        std::ranges::copy(std::span<const float>{buffer}.first(produced.has_value() ? produced.value() : 0U), std::back_inserter(output));
    }

    return output;
}

[[nodiscard]] std::vector<float> noise_input(std::size_t frames)
{
    sl::det::Pcg32 generator{input_seed, 0U, 0U};
    std::vector<float> input(frames);

    for (float& sample : input)
    {
        sample = sl::to_float((generator.uniform() - input_centre) * input_scale);
    }

    return input;
}

[[nodiscard]] std::string_view text_of(const char* text)
{
    return text == nullptr ? std::string_view{} : std::string_view{text};
}

} // namespace

TEST_CASE("watterson engine: at most one Watterson stage, and it must be first (U26)", "[watterson][engine]")
{
    auto scenario = std::make_unique<sl::Scenario>();
    const m110::Status noise_first = sl::parse_scenario(after_noise.data(), after_noise.size(), *scenario);
    CHECK(noise_first.code == m110::StatusCode::invalid_configuration);
    CHECK(text_of(noise_first.message) == first_message);
    const m110::Status two_stages = sl::parse_scenario(twice.data(), twice.size(), *scenario);
    CHECK(two_stages.code == m110::StatusCode::invalid_configuration);
    CHECK(text_of(two_stages.message) == single_message);
    REQUIRE(sl::parse_scenario(then_noise.data(), then_noise.size(), *scenario).is_ok());
    CHECK(scenario->stage_count == 2U);
    CHECK(scenario->stages[0].type == sl::StageType::watterson);
    CHECK(std::string_view{sl::stage_type_name(sl::StageType::watterson)} == watterson_name);
    // Programmatic scenarios are held to the same rules.
    scenario->stages[1].type = sl::StageType::watterson;
    CHECK(text_of(sl::validate_scenario(*scenario).message) == single_message);
    scenario->stages[0].type = sl::StageType::awgn;
    const m110::Status not_first = sl::validate_scenario(*scenario);
    CHECK(not_first.code == m110::StatusCode::invalid_configuration);
    CHECK(text_of(not_first.message) == first_message);
}

TEST_CASE("watterson engine: without StageResources a Watterson scenario fails configure() and says why (U27)", "[watterson][engine]")
{
    auto scenario = std::make_unique<sl::Scenario>();
    auto engine = std::make_unique<sl::Engine>();
    REQUIRE(sl::parse_scenario(ccir_poor.data(), ccir_poor.size(), *scenario).is_ok());
    const m110::Status configured = engine->configure(*scenario, reference_rms, total_frames);
    CHECK(configured.code == m110::StatusCode::invalid_configuration);
    CHECK(text_of(configured.message) == channel_message);
    CHECK_FALSE(engine->configured());
    CHECK(engine->watterson_report() == nullptr);
}

TEST_CASE("watterson engine: the stage is in its typed slot, the arena holds the legacy stages as before (U28)", "[watterson][engine]")
{
    const sl::StageResources resources{&shared_channel(), nullptr};
    const Configured mixed = configure(with_legacy, resources);
    REQUIRE(mixed.ok);
    const Configured legacy = configure(legacy_only, resources);
    REQUIRE(legacy.ok);
    CHECK(mixed.engine->stats().arena_bytes_used == legacy.engine->stats().arena_bytes_used);
    CHECK(mixed.engine->stats().arena_bytes_used == sl::impairment_size(sl::StageType::cw) + sl::impairment_size(sl::StageType::impulse));
    CHECK(sl::impairment_size(sl::StageType::watterson) == 0U);
    REQUIRE(mixed.engine->stage(0U) != nullptr);
    CHECK(mixed.engine->stage(0U)->type() == sl::StageType::watterson);
    const sl::WattersonReport* report = mixed.engine->watterson_report();
    REQUIRE(report != nullptr);
    CHECK(report->path_count == 2U);
    CHECK(report->paths[1].delay.value() == two_ms_frames);
    CHECK(report->latency == sl::hilbert_latency);
    CHECK(legacy.engine->watterson_report() == nullptr);
}

TEST_CASE("watterson engine: the stage parser and the full parser give the same verdicts (U36)", "[watterson][engine][schema]")
{
    auto document = std::make_unique<sl::json::Document>();
    auto scenario = std::make_unique<sl::Scenario>();

    for (const std::string_view stage : stage_refusals)
    {
        REQUIRE(document->parse(stage.data(), stage.size()).is_ok());
        sl::WattersonParams params{};
        const m110::Status direct = sl::parse_watterson_stage(*document, *document->root(), params);
        const std::string full = std::string{R"({"impairments": [)"} + std::string{stage} + "]}";
        INFO(stage);
        CHECK_FALSE(direct.is_ok());
        const m110::Status parsed = sl::parse_scenario(full.data(), full.size(), *scenario);
        CHECK(parsed.code == direct.code);
        CHECK(text_of(parsed.message) == std::string_view{direct.message});
    }
}

TEST_CASE("watterson engine: an impulse at frame k comes out at k + D + L at every block size (E.4.4, algorithmic)", "[watterson][engine]")
{
    const sl::StageResources resources{&shared_channel(), nullptr};
    std::vector<float> input(impulse_frames, 0.0F);
    input[impulse_frame] = impulse_height;

    for (const std::size_t block : latency_blocks)
    {
        const Configured configured = configure(fixed_two_ms, resources);
        REQUIRE(configured.ok);
        bool ok = false;
        const std::vector<float> output = render(*configured.engine, input, block, ok);
        REQUIRE(ok);
        const std::size_t expected = impulse_frame + sl::hilbert_delay + two_ms_frames;
        const auto peak = std::ranges::max_element(output, [](float a, float b) { return std::abs(a) < std::abs(b); });

        INFO("block " << block);
        CHECK(signal_lab_test::count_of(std::distance(output.begin(), peak)) == expected);
        CHECK(*peak == impulse_height);
        CHECK(std::ranges::count_if(output, [](float sample) { return sample != 0.0F; }) == 1);
    }
}

TEST_CASE("watterson engine: the engine's output is the channel's, bit for bit, at any block size", "[watterson][engine]")
{
    const sl::StageResources resources{&shared_channel(), nullptr};
    const std::vector<float> input = noise_input(render_frames);
    const Configured configured = configure(ccir_poor, resources);
    REQUIRE(configured.ok);
    bool ok = false;
    const std::vector<float> through_engine = render(*configured.engine, input, engine_block, ok);
    REQUIRE(ok);
    const std::uint64_t parameters = configured.engine->watterson_report()->parameter_digest;
    // The same channel, driven directly with the engine's seed and stage index.
    auto direct = std::make_unique<sl::WattersonChannel>();
    REQUIRE(direct->configure(configured.scenario->watterson, ccir_seed, 0U, nullptr).is_ok());
    std::vector<float> through_channel = input;

    for (std::size_t offset = 0U; offset < through_channel.size(); offset += engine_block)
    {
        const std::size_t count = std::min(engine_block, through_channel.size() - offset);
        REQUIRE(direct->process(std::span{through_channel}.subspan(offset, count), units::SampleIndex{offset}).is_ok());
    }

    CHECK(direct->report().parameter_digest == parameters);
    CHECK(std::ranges::equal(through_engine, through_channel, [](float a, float b) { return std::bit_cast<std::uint32_t>(a) == std::bit_cast<std::uint32_t>(b); }));

    const Configured again = configure(ccir_poor, resources);
    REQUIRE(again.ok);
    constexpr std::size_t odd_block = 1000U;
    CHECK(render(*again.engine, input, odd_block, ok) == through_engine);
    CHECK(ok);
}

TEST_CASE("watterson engine: a channel fault is the engine's latched Status, with the channel's own message", "[watterson][engine]")
{
    const sl::StageResources resources{&shared_channel(), nullptr};
    const std::vector<float> input = noise_input(engine_block);
    std::vector<float> output(sl::engine_capacity_frames);
    const Configured configured = configure(ccir_poor, resources);
    REQUIRE(configured.ok);
    const m110::Result<std::size_t> first = configured.engine->process(input, output);
    REQUIRE(first.has_value());
    CHECK(first.value() == engine_block);
    CHECK(configured.engine->status().is_ok());
    // A refused configure() of the shared channel leaves it unconfigured under the engine.
    REQUIRE_FALSE(shared_channel().configure(sl::WattersonParams{}, ccir_seed, 0U, nullptr).is_ok());
    const m110::Result<std::size_t> faulted = configured.engine->process(input, output);
    REQUIRE_FALSE(faulted.has_value());
    CHECK(faulted.error().code == m110::StatusCode::invalid_configuration);
    CHECK(text_of(faulted.error().message) == unconfigured_message);
    CHECK_FALSE(sl::is_engine_failure(faulted.error()));
    CHECK(sl::same_failure(configured.engine->status(), faulted.error()));
    // The latch holds: the next block returns the same Status without reaching the stage.
    const m110::Result<std::size_t> latched = configured.engine->process(input, output);
    REQUIRE_FALSE(latched.has_value());
    CHECK(sl::same_failure(latched.error(), faulted.error()));
    // configure() clears the fault and the status.
    const Configured again = configure(ccir_poor, resources);
    REQUIRE(again.ok);
    CHECK(again.engine->status().is_ok());
}

TEST_CASE("watterson engine: a non-contiguous block through the adapter is a Status, not a count", "[watterson][engine]")
{
    auto scenario = std::make_unique<sl::Scenario>();
    REQUIRE(sl::parse_scenario(ccir_poor.data(), ccir_poor.size(), *scenario).is_ok());
    std::vector<float> scratch(sl::engine_capacity_frames);
    sl::WattersonStage stage{scenario->watterson, &shared_channel(), nullptr};
    REQUIRE(stage.prepare(sl::StageContext{reference_rms, total_frames, ccir_seed, 0U, scratch.data()}).is_ok());
    std::vector<float> block = noise_input(engine_block);
    constexpr std::size_t unbounded_capacity = std::numeric_limits<std::size_t>::max();
    const m110::Result<std::size_t> contiguous = stage.process(block.data(), block.size(), 0U, unbounded_capacity);
    REQUIRE(contiguous.has_value());
    CHECK(contiguous.value() == block.size());
    // Frame engine_block + 1 does not follow frame engine_block - 1.
    const m110::Result<std::size_t> gap = stage.process(block.data(), block.size(), engine_block + 1U, unbounded_capacity);
    REQUIRE_FALSE(gap.has_value());
    CHECK(gap.error().code == m110::StatusCode::invalid_argument);
    CHECK(text_of(gap.error().message) == contiguity_message);
    // The channel latches the fault until it is configured again.
    CHECK_FALSE(stage.process(block.data(), block.size(), engine_block, unbounded_capacity).has_value());
    REQUIRE(stage.prepare(sl::StageContext{reference_rms, total_frames, ccir_seed, 0U, scratch.data()}).is_ok());
    CHECK(stage.process(block.data(), block.size(), 0U, unbounded_capacity).has_value());
}

TEST_CASE("watterson engine: the noise after the channel does not change the channel", "[watterson][engine]")
{
    const sl::StageResources resources{&shared_channel(), nullptr};
    const Configured first = configure(then_noise, resources);
    REQUIRE(first.ok);
    const std::uint64_t digest = first.engine->watterson_report()->parameter_digest;
    constexpr std::string_view quieter = R"({"impairments": [{"type": "watterson", "paths": [{"spread_hz": 1}]}, {"type": "awgn", "snr_db": 40}]})";
    const Configured second = configure(quieter, resources);
    REQUIRE(second.ok);
    CHECK(second.engine->watterson_report()->parameter_digest == digest);
    CHECK(second.engine->stage(1U)->type() == sl::StageType::awgn);
}
