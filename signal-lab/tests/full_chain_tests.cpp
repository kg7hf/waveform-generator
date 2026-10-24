// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// Maximum legal stage composition across the parser, typed slots and arena.

#include "allocation_counter.hpp"
#include "signal_lab/mixer.hpp"
#include "signal_lab/scenario.hpp"
#include "signal_lab/secondary_source.hpp"
#include "signal_lab/watterson.hpp"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace sl = signal_lab;

namespace
{

constexpr std::size_t frame_count = 12'000U;
constexpr double pi = 3.141592653589793238462643383279502884;
constexpr std::string_view nine_stage_scenario = R"({
  "seed": 314159,
  "source_gain_db": 0,
  "clip_policy": "reject",
  "impairments": [
    {"type":"watterson","stream_id":101,"paths":[{"fading":"rayleigh","spread_hz":1.0}]},
    {"type":"frequency_shift","stream_id":102,"mode":"offset","offset_hz":25},
    {"type":"second_source","stream_id":103,"source_path":"secondary.wav","si_db":12,"duration_frames":12000,"fading":{"spread_hz":2.0},"markov_on_off":{"mean_on_seconds":0.002,"mean_off_seconds":0.003}},
    {"type":"awgn","stream_id":104,"snr_db":30},
    {"type":"awgn","stream_id":105,"snr_db":32},
    {"type":"cw","stream_id":106,"frequency_hz":1000,"ci_db":18,"fsk":{"baud":300,"shift_hz":200,"pattern":"random"},"markov_on_off":{"mean_on_seconds":0.002,"mean_off_seconds":0.003}},
    {"type":"impulse","stream_id":107,"ring":"noise","rate_per_sec":30,"peak_db":-30,"decay_ms":8},
    {"type":"fade","stream_id":108,"depth_db":3,"duration_ms":5,"period_seconds":0.05,"count":4},
    {"type":"sample_slip","stream_id":109,"kind":"delete","length_samples":3,"at_seconds":0.1}
  ]
})";

struct Render
{
    std::vector<float> samples{};
    sl::EngineStats stats{};
    m110::Status configured{};
    bool processed{};
    std::uint64_t allocations{};
};

[[nodiscard]] double rms(std::span<const float> samples)
{
    double energy{};

    for (const float sample : samples)
    {
        energy += static_cast<double>(sample) * sample;
    }

    return std::sqrt(energy / static_cast<double>(samples.size()));
}

[[nodiscard]] std::vector<float> make_tone(std::size_t frames, double frequency_hz, double amplitude)
{
    std::vector<float> samples(frames);

    for (std::size_t frame = 0U; frame < frames; ++frame)
    {
        const double phase = 2.0 * pi * frequency_hz * static_cast<double>(frame) / static_cast<double>(sl::stream_rate.hertz());
        samples[frame] = static_cast<float>(amplitude * std::sin(phase));
    }

    return samples;
}

[[nodiscard]] Render render_partition(sl::Engine& engine, const sl::Scenario& scenario, const sl::StageResources& resources, std::span<const float> input, double input_rms, std::size_t block_frames)
{
    Render result{};
    result.samples.resize(input.size() + sl::engine_slack_frames);
    std::size_t output_frames{};
    {
        signal_lab_test::CountedRegion counted{};
        result.configured = engine.configure(scenario, input_rms, input.size(), resources);
        result.processed = result.configured.is_ok();

        for (std::size_t first = 0U; result.processed && first < input.size(); first += block_frames)
        {
            const std::size_t count = std::min(block_frames, input.size() - first);
            const auto available = std::span<float>{result.samples}.subspan(output_frames, count + sl::engine_slack_frames);
            const auto produced = engine.process(input.subspan(first, count), available);

            if (!produced.has_value())
            {
                result.processed = false;
                break;
            }

            output_frames += produced.value();
        }

        result.allocations = counted.allocations();
    }

    result.stats = engine.stats();
    result.samples.resize(output_frames);
    return result;
}

[[nodiscard]] bool same_samples(std::span<const float> left, std::span<const float> right)
{
    return left.size() == right.size() && std::equal(left.begin(), left.end(), right.begin());
}

[[nodiscard]] std::string ten_stage_scenario()
{
    std::string text = R"({"impairments":[)";

    for (std::size_t index = 0U; index < sl::max_stages + 1U; ++index)
    {
        if (index != 0U)
        {
            text += ',';
        }

        text += R"({"type":"awgn","snr_db":30})";
    }

    text += "]}";
    return text;
}

} // namespace

TEST_CASE("nine-stage full chain configures without allocation and is partition invariant", "[engine][full-chain][allocation]")
{
    auto scenario = std::make_unique<sl::Scenario>();
    REQUIRE(sl::parse_scenario(nine_stage_scenario.data(), nine_stage_scenario.size(), *scenario).is_ok());
    REQUIRE(scenario->stage_count == sl::max_stages);
    REQUIRE(sl::validate_scenario(*scenario).is_ok());
    constexpr std::array expected_order{sl::StageType::watterson, sl::StageType::frequency_shift, sl::StageType::second_source, sl::StageType::awgn,       sl::StageType::awgn,
                                        sl::StageType::cw,        sl::StageType::impulse,         sl::StageType::fade,          sl::StageType::sample_slip};

    for (std::size_t index = 0U; index < expected_order.size(); ++index)
    {
        CHECK(scenario->stages[index].type == expected_order[index]);
    }

    auto channel = std::make_unique<sl::WattersonChannel>();
    std::vector<float> secondary = make_tone(frame_count, 1500.0, 0.0025);
    sl::FixedSecondarySource provider{secondary, rms(secondary)};
    sl::StageResources resources{};
    resources.watterson = channel.get();
    resources.secondary_source = &provider;
    const std::vector<float> input = make_tone(frame_count, 1200.0, 0.01);
    const double input_rms = rms(input);
    sl::Engine engine{};
    const Render by_one = render_partition(engine, *scenario, resources, input, input_rms, 1U);
    const Render by_127 = render_partition(engine, *scenario, resources, input, input_rms, 127U);
    const Render by_2048 = render_partition(engine, *scenario, resources, input, input_rms, 2048U);
    const Render replay = render_partition(engine, *scenario, resources, input, input_rms, 2048U);

    for (const Render* rendered : {&by_one, &by_127, &by_2048, &replay})
    {
        INFO("configure status: " << rendered->configured.message);
        REQUIRE(rendered->configured.is_ok());
        CHECK(rendered->processed);
        CHECK(rendered->allocations == 0U);
        CHECK(rendered->stats.stage_count == sl::max_stages);
        CHECK(rendered->stats.frames_in == frame_count);
        CHECK(rendered->stats.frames_out == frame_count - 3U);
        CHECK(rendered->stats.events_applied >= 1U);
        CHECK(rendered->stats.clipped_samples == 0U);
        CHECK(std::isfinite(rendered->stats.peak));
        CHECK(rendered->stats.peak < 1.0F);
        CHECK(std::all_of(rendered->samples.begin(), rendered->samples.end(), [](float sample) { return std::isfinite(sample); }));
    }

    CHECK(same_samples(by_one.samples, by_127.samples));
    CHECK(same_samples(by_one.samples, by_2048.samples));
    CHECK(same_samples(by_2048.samples, replay.samples));
    CHECK(by_one.stats.output_digest == by_127.stats.output_digest);
    CHECK(by_one.stats.output_digest == by_2048.stats.output_digest);
    CHECK(by_2048.stats.output_digest == replay.stats.output_digest);
}

TEST_CASE("scenario parser refuses a tenth impairment stage", "[scenario][full-chain]")
{
    auto scenario = std::make_unique<sl::Scenario>();
    const std::string text = ten_stage_scenario();
    const m110::Status parsed = sl::parse_scenario(text.data(), text.size(), *scenario);
    REQUIRE_FALSE(parsed.is_ok());
    REQUIRE(parsed.message != nullptr);
    CHECK(std::string_view{parsed.message}.find("maximum 9") != std::string_view::npos);
}

TEST_CASE("scenario parser reinitializes a reused nine-stage descriptor", "[scenario][full-chain][reset]")
{
    auto scenario = std::make_unique<sl::Scenario>();
    REQUIRE(sl::parse_scenario(nine_stage_scenario.data(), nine_stage_scenario.size(), *scenario).is_ok());
    REQUIRE(scenario->stage_count == sl::max_stages);
    REQUIRE(scenario->watterson.path_count == 1U);
    REQUIRE(scenario->frequency_shift.offset.in(sl::units::Hz) == 25.0);
    REQUIRE(scenario->second_source.fading.enabled);
    REQUIRE(scenario->second_source.markov.enabled);
    REQUIRE(scenario->stages[5].cw.fsk.enabled);
    REQUIRE(scenario->stages[5].cw.markov.enabled);
    REQUIRE(scenario->stages[6].impulse.ring_noise);
    REQUIRE(scenario->stages[7].fade.depth_db == 3.0);
    REQUIRE(scenario->stages[8].slip.event_count == 1U);
    constexpr std::string_view minimal = "{}";
    m110::Status parsed{};
    std::uint64_t allocations{};
    {
        signal_lab_test::CountedRegion counted{};
        parsed = sl::parse_scenario(minimal.data(), minimal.size(), *scenario);
        allocations = counted.allocations();
    }

    REQUIRE(parsed.is_ok());
    CHECK(allocations == 0U);
    CHECK(scenario->seed == 0U);
    CHECK(scenario->source_gain_db == -12.0);
    CHECK(scenario->saturate);
    CHECK(scenario->reference_auto);
    CHECK_FALSE(scenario->has_reference_rms);
    CHECK(scenario->reference_rms == 0.0);
    CHECK(scenario->noise_bandwidth == sl::NoiseBandwidth::khz3);
    CHECK(scenario->stage_count == 0U);
    CHECK(scenario->watterson.path_count == 0U);
    CHECK(scenario->frequency_shift.mode == sl::FrequencyShiftMode::offset);
    CHECK(scenario->frequency_shift.offset.in(sl::units::Hz) == 0.0);
    CHECK(scenario->frequency_shift.amplitude.in(sl::units::Hz) == 75.0);
    CHECK_FALSE(scenario->second_source.fading.enabled);
    CHECK_FALSE(scenario->second_source.markov.enabled);
    CHECK(scenario->second_source.si.in(sl::units::dB) == 0.0);
    CHECK(scenario->second_source.duration_frames == 0U);
    CHECK(scenario->second_source_path[0] == '\0');

    for (const sl::Stage& stage : scenario->stages)
    {
        CHECK(stage.type == sl::StageType::awgn);
        CHECK(stage.stream_id == 0U);
        CHECK(stage.awgn.snr.in(sl::units::dB) == 0.0);
        CHECK(stage.cw.frequency_hz == 0.0);
        CHECK_FALSE(stage.cw.fsk.enabled);
        CHECK_FALSE(stage.cw.markov.enabled);
        CHECK_FALSE(stage.cw.fading.enabled);
        CHECK(stage.impulse.peak_db == 0.0);
        CHECK(stage.impulse.decay_ms == 0.0);
        CHECK_FALSE(stage.impulse.ring_noise);
        CHECK(stage.fade.depth_db == 0.0);
        CHECK(stage.slip.event_count == 0U);
        CHECK(stage.slip.events[0].length_samples == 0U);
    }
}
