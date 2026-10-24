// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// Catch2 suite of Criterion 8's "no allocation on the audio path": the counting
// global operator new of this binary (allocation_counter.hpp) observes
// WattersonChannel::configure() and process(), and Engine::configure() and
// process() for a Watterson + AWGN scenario, and finds no allocation in any.
// The static_asserts beside the types (trivially copyable, sized) say the state
// is static; these say the code does not reach the heap either.
// Guide: docs/modules/signal-lab-watterson.md, "Memory and timing".

#include "allocation_counter.hpp"
#include "signal_lab/det_math.hpp"
#include "signal_lab/mixer.hpp"
#include "signal_lab/scenario.hpp"
#include "signal_lab/units/sampling.hpp"
#include "signal_lab/watterson.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace
{

namespace sl = signal_lab;
namespace units = signal_lab::units;

// Every feature on the audio path: two Rayleigh paths, a shifted fixed path with a
// phase, a Rayleigh path at 20 ms, then AWGN after the channel.
constexpr std::string_view scenario_text = R"({"seed": 3, "source_gain_db": -12, "impairments": [)"
                                           R"({"type": "watterson", "paths": [{"spread_hz": 1}, {"delay_ms": 2, "spread_hz": 5},)"
                                           R"( {"delay_ms": 9, "fading": "fixed", "shift_hz": -75, "phase_deg": 30}, {"delay_ms": 20, "spread_hz": 0.1}]},)"
                                           R"( {"type": "awgn", "snr_db": 10}]})";
constexpr double reference_rms = 0.25;
constexpr std::uint64_t stream_frames = 96000U;
constexpr std::size_t render_blocks = 24U;
// Block sizes of the engine (2048) and the partitions of U21: odd, chunk-straddling.
constexpr std::size_t engine_block = 2048U;
constexpr std::size_t odd_block = 1001U;
constexpr std::uint64_t input_seed = 31U;
constexpr double input_centre = 0.5;

[[nodiscard]] std::vector<float> uniform_input(std::size_t frames)
{
    sl::det::Pcg32 generator{input_seed, 0U, 0U};
    std::vector<float> input(frames);

    for (float& sample : input)
    {
        sample = sl::to_float(generator.uniform() - input_centre);
    }

    return input;
}

[[nodiscard]] sl::WattersonChannel& channel()
{
    constinit static sl::WattersonChannel instance{};
    return instance;
}

} // namespace

TEST_CASE("allocation: the Watterson channel configures and processes without the heap", "[allocation][watterson]")
{
    auto scenario = std::make_unique<sl::Scenario>();
    REQUIRE(sl::parse_scenario(scenario_text.data(), scenario_text.size(), *scenario).is_ok());
    std::vector<float> block = uniform_input(odd_block * render_blocks);
    bool all_ok = true;
    std::uint64_t allocations = 0U;
    {
        const signal_lab_test::CountedRegion region;
        all_ok = channel().configure(scenario->watterson, scenario->seed, 0U, nullptr).is_ok();

        for (std::size_t offset = 0U; all_ok && offset < block.size(); offset += odd_block)
        {
            all_ok = channel().process(std::span{block}.subspan(offset, odd_block), units::SampleIndex{offset}).is_ok();
        }

        allocations = region.allocations();
    }

    CHECK(all_ok);
    CHECK(allocations == 0U);
}

TEST_CASE("allocation: the engine configures and processes a Watterson + AWGN scenario without the heap", "[allocation][engine]")
{
    auto scenario = std::make_unique<sl::Scenario>();
    auto engine = std::make_unique<sl::Engine>();
    REQUIRE(sl::parse_scenario(scenario_text.data(), scenario_text.size(), *scenario).is_ok());
    const sl::StageResources resources{&channel(), nullptr};
    const std::vector<float> input = uniform_input(engine_block);
    std::vector<float> output(sl::engine_capacity_frames);
    bool all_ok = true;
    std::uint64_t allocations = 0U;
    {
        const signal_lab_test::CountedRegion region;
        all_ok = engine->configure(*scenario, reference_rms, stream_frames, resources).is_ok();

        for (std::size_t index = 0U; all_ok && index < render_blocks; ++index)
        {
            const m110::Result<std::size_t> produced = engine->process(std::span<const float>{input}, std::span<float>{output});
            all_ok = produced.has_value() && produced.value() == engine_block;
        }

        allocations = region.allocations();
    }

    CHECK(all_ok);
    CHECK(engine->status().is_ok());
    CHECK(allocations == 0U);
}

TEST_CASE("allocation: the counter sees an allocation inside a region (positive control)", "[allocation]")
{
    std::uint64_t allocations = 0U;
    {
        const signal_lab_test::CountedRegion region;
        const auto value = std::make_unique<std::uint64_t>(1U);
        signal_lab_test::retain(value.get());
        allocations = region.allocations();
        signal_lab_test::retain(nullptr);
    }

    CHECK(allocations == 1U);
}
