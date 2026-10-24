// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// Scenario and engine integration for the global Doppler stage.
// Guide: docs/modules/signal-lab-engine.md.

#include "allocation_counter.hpp"
#include "signal_lab/mixer.hpp"
#include "signal_lab/scenario.hpp"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace
{

namespace sl = signal_lab;
constexpr double reference_rms = 0.125;

[[nodiscard]] m110::Status parse(std::string_view text, sl::Scenario& scenario)
{
    return sl::parse_scenario(text.data(), text.size(), scenario);
}

[[nodiscard]] std::vector<float> render(sl::Engine& engine, std::span<const float> input, std::size_t partition)
{
    std::array<float, sl::engine_capacity_frames> block{};
    std::vector<float> output;

    for (std::size_t offset = 0U; offset < input.size(); offset += partition)
    {
        const auto count = std::min(partition, input.size() - offset);
        const auto result = engine.process(input.subspan(offset, count), block);
        REQUIRE(result.has_value());
        REQUIRE(result.value() == count);
        output.insert(output.end(), block.begin(), block.begin() + static_cast<std::ptrdiff_t>(count));
    }

    return output;
}

} // namespace

TEST_CASE("frequency shift: strict JSON modes and sweep defaults", "[frequency_shift][scenario]")
{
    auto scenario = std::make_unique<sl::Scenario>();
    REQUIRE(parse(R"({"impairments":[{"type":"frequency_shift","mode":"offset","offset_hz":-75}]})", *scenario).is_ok());
    CHECK(scenario->stages[0].type == sl::StageType::frequency_shift);
    CHECK(scenario->frequency_shift.offset.hertz() == -75.0);
    CHECK(std::string_view{sl::stage_type_name(sl::StageType::frequency_shift)} == "frequency_shift");
    REQUIRE(parse(R"({"impairments":[{"type":"frequency_shift","mode":"sweep"}]})", *scenario).is_ok());
    CHECK(scenario->frequency_shift.amplitude.hertz() == 75.0);
    CHECK(scenario->frequency_shift.slope.hertz_per_second() == 3.5);
    CHECK(scenario->frequency_shift.start_frequency.hertz() == -75.0);
    CHECK(scenario->frequency_shift.direction == sl::TriangleDirection::ascending);
    REQUIRE(parse(R"({"impairments":[{"type":"frequency_shift","mode":"sweep","amplitude_hz":25}]})", *scenario).is_ok());
    CHECK(scenario->frequency_shift.start_frequency.hertz() == -25.0);
    REQUIRE(parse(R"({"impairments":[{"type":"frequency_shift","mode":"sweep","start_frequency_hz":30,"direction":"descending"}]})", *scenario).is_ok());
    CHECK(scenario->frequency_shift.start_frequency.hertz() == 30.0);
    CHECK(scenario->frequency_shift.direction == sl::TriangleDirection::descending);
    constexpr std::array refused{R"({"type":"frequency_shift"})",
                                 R"({"type":"frequency_shift","mode":"offset"})",
                                 R"({"type":"frequency_shift","mode":"offset","offset_hz":true})",
                                 R"({"type":"frequency_shift","mode":"offset","offset_hz":0,"direction":"ascending"})",
                                 R"({"type":"frequency_shift","mode":"offset","offset_hz":75,"offset_hz":-75})",
                                 R"({"type":"frequency_shift","mode":"sweep","mode":"offset","offset_hz":0})",
                                 R"({"type":"frequency_shift","mode":"sweep","offset_hz":0})",
                                 R"({"type":"frequency_shift","mode":"sweep","direction":"up"})",
                                 R"({"type":"frequency_shift","mode":"sweep","start_frequency_hz":76})",
                                 R"({"type":"frequency_shift","mode":"sweep","amplitude_hz":0})",
                                 R"({"type":"frequency_shift","mode":"sweep","slope_hz_per_second":0})",
                                 R"({"type":"frequency_shift","mode":"sweep","slope_hz_per_second":"3.5"})",
                                 R"({"type":"frequency_shift","mode":"sweep","amplitude_hz":true})",
                                 R"({"type":"frequency_shift","mode":"sweep","start_frequency_hz":null})",
                                 R"({"type":"frequency_shift","mode":"sweep","start_seconds":1})",
                                 R"({"type":"frequency_shift","mode":"unknown"})"};

    for (const auto* stage : refused)
    {
        CAPTURE(stage);
        CHECK_FALSE(parse(std::string{"{\"impairments\": ["} + stage + "]}", *scenario).is_ok());
    }
}

TEST_CASE("frequency shift: one stage before noise and after the channel", "[frequency_shift][scenario]")
{
    auto scenario = std::make_unique<sl::Scenario>();
    constexpr std::string_view valid = R"({"impairments":[{"type":"watterson","paths":[{"fading":"fixed"}]},{"type":"frequency_shift","mode":"sweep"},{"type":"awgn","snr_db":24}]})";
    REQUIRE(parse(valid, *scenario).is_ok());
    std::swap(scenario->stages[1], scenario->stages[2]);
    CHECK_FALSE(sl::validate_scenario(*scenario).is_ok());
    REQUIRE(parse(valid, *scenario).is_ok());
    scenario->stages[2].type = sl::StageType::frequency_shift;
    CHECK_FALSE(sl::validate_scenario(*scenario).is_ok());
    REQUIRE(parse(valid, *scenario).is_ok());
    std::swap(scenario->stages[0], scenario->stages[1]);
    CHECK_FALSE(sl::validate_scenario(*scenario).is_ok());
    REQUIRE(parse(R"({"impairments":[{"type":"frequency_shift","mode":"offset","offset_hz":0}]})", *scenario).is_ok());
    scenario->frequency_shift.offset = sl::units::FrequencyDelta{10000.0, sl::units::Hz};
    CHECK_FALSE(sl::validate_scenario(*scenario).is_ok());
}

TEST_CASE("frequency shift: independent Hilbert delay composes after Watterson", "[frequency_shift][engine]")
{
    auto scenario = std::make_unique<sl::Scenario>();
    auto engine = std::make_unique<sl::Engine>();
    auto channel = std::make_unique<sl::WattersonChannel>();
    REQUIRE(
        parse(R"({"source_gain_db":0,"impairments":[{"type":"watterson","paths":[{"fading":"fixed","delay_ms":2}]},{"type":"frequency_shift","mode":"offset","offset_hz":0}]})", *scenario).is_ok());
    constexpr std::size_t input_frame = 10U;
    constexpr std::size_t channel_delay_frames = 96U;
    constexpr std::size_t output_frame = input_frame + channel_delay_frames + std::size_t{2U} * sl::hilbert_delay;
    std::vector<float> input(output_frame + 50U, 0.0F);
    input[input_frame] = 0.5F;
    REQUIRE(engine->configure(*scenario, reference_rms, input.size(), {channel.get(), nullptr}).is_ok());
    const auto output = render(*engine, input, 63U);

    for (std::size_t frame = 0U; frame < output.size(); ++frame)
    {
        CHECK(output[frame] == (frame == output_frame ? 0.5F : 0.0F));
    }

    engine->configure_passthrough();
    CHECK(render(*engine, input, 71U) == input);
}

TEST_CASE("frequency shift: full chain is partition independent and allocation free", "[frequency_shift][engine][allocation]")
{
    auto scenario = std::make_unique<sl::Scenario>();
    auto engine = std::make_unique<sl::Engine>();
    auto channel = std::make_unique<sl::WattersonChannel>();
    REQUIRE(
        parse(
            R"({"source_gain_db":-12,"seed":3,"impairments":[{"type":"watterson","paths":[{"spread_hz":1},{"spread_hz":1,"delay_ms":2}]},{"type":"frequency_shift","mode":"sweep"},{"type":"awgn","snr_db":24}]})",
            *scenario)
            .is_ok());
    constexpr std::size_t frames = 5001U;
    std::vector<float> input(frames);
    sl::det::Pcg32 random{42U, 0U, 0U};

    for (float& sample : input)
    {
        sample = static_cast<float>(random.uniform() - 0.5);
    }

    const sl::StageResources resources{channel.get(), nullptr};
    REQUIRE(engine->configure(*scenario, reference_rms, frames, resources).is_ok());
    const auto expected = render(*engine, input, sl::engine_block_frames);
    const auto digest = engine->stats().output_digest;

    for (const std::size_t partition : {1U, 63U, 65U, 1001U})
    {
        CAPTURE(partition);
        REQUIRE(engine->configure(*scenario, reference_rms, frames, resources).is_ok());
        CHECK(render(*engine, input, partition) == expected);
        CHECK(engine->stats().output_digest == digest);
    }

    std::array<float, sl::engine_capacity_frames> output{};
    bool prepared = false;
    bool processed = false;
    std::uint64_t allocations = 0U;
    {
        const signal_lab_test::CountedRegion region;
        prepared = engine->configure(*scenario, reference_rms, frames, resources).is_ok();
        processed = engine->process(std::span{input}.first(sl::engine_block_frames), output).has_value();
        allocations = region.allocations();
    }

    CHECK(prepared);
    CHECK(processed);
    CHECK(allocations == 0U);
}

TEST_CASE("frequency shift: channel and Doppler retain the 3 kHz AWGN reference", "[frequency_shift][engine][snr]")
{
    auto scenario = std::make_unique<sl::Scenario>();
    auto engine = std::make_unique<sl::Engine>();
    auto channel = std::make_unique<sl::WattersonChannel>();
    // The fixed unit-gain path isolates the added shifter from random fading.
    // Paired renders subtract the identical clean signal; ENBW converts the
    // measured full noise-band power to the specification's 3000 Hz reference.
    constexpr std::size_t warmup = 4096U;
    constexpr std::size_t measured_frames = std::size_t{8U} * sl::sample_rate_hz;
    constexpr double wanted_snr_db = 24.0;
    std::vector<float> input(warmup + measured_frames);

    for (std::size_t frame = 0U; frame < input.size(); ++frame)
    {
        const double angle = sl::det::two_pi * 1800.0 * static_cast<double>(frame) / sl::sample_rate_hz;
        input[frame] = static_cast<float>(std::sqrt(2.0) * reference_rms * std::cos(angle));
    }

    constexpr std::array shifts{R"({"type":"frequency_shift","mode":"offset","offset_hz":75})", R"({"type":"frequency_shift","mode":"offset","offset_hz":-75})",
                                R"({"type":"frequency_shift","mode":"sweep"})"};

    for (const auto* shift : shifts)
    {
        CAPTURE(shift);
        const std::string prefix = std::string{R"({"source_gain_db":0,"seed":313,"impairments":[{"type":"watterson","paths":[{"fading":"fixed"}]},)"} + shift;
        REQUIRE(parse(prefix + "]}", *scenario).is_ok());
        REQUIRE(engine->configure(*scenario, reference_rms, input.size(), {channel.get(), nullptr}).is_ok());
        const auto clean = render(*engine, input, 1024U);
        // Reconfigure only after the previous render: Engine borrows Scenario.
        REQUIRE(parse(prefix + R"(,{"type":"awgn","snr_db":24}]})", *scenario).is_ok());
        REQUIRE(engine->configure(*scenario, reference_rms, input.size(), {channel.get(), nullptr}).is_ok());
        const auto noisy = render(*engine, input, 1024U);
        double signal_energy = 0.0;
        double noise_energy = 0.0;

        for (std::size_t frame = warmup; frame < input.size(); ++frame)
        {
            const double signal = clean[frame];
            const double noise = static_cast<double>(noisy[frame]) - signal;
            signal_energy += signal * signal;
            noise_energy += noise * noise;
        }

        const double noise_3khz = noise_energy * 3000.0 / sl::noise_band_kernel().enbw.hertz();
        const double measured_snr_db = 10.0 * std::log10(signal_energy / noise_3khz);
        const double signal_gain_db = 10.0 * std::log10(signal_energy / (measured_frames * reference_rms * reference_rms));
        CAPTURE(measured_snr_db, signal_gain_db);
        CHECK(std::abs(measured_snr_db - wanted_snr_db) < 0.1);
        CHECK(std::abs(signal_gain_db) < 0.1);
    }
}
