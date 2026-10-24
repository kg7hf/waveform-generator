// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

#include "allocation_counter.hpp"
#include "signal_lab/cw_fading.hpp"
#include "signal_lab/det_math.hpp"
#include "signal_lab/impairment.hpp"
#include "signal_lab/scenario.hpp"
#include "signal_lab/watterson.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace sl = signal_lab;

namespace
{

constexpr std::uint64_t seed = 0x12345678U;
constexpr std::uint32_t stage_index = 3U;

struct StageStorage
{
    alignas(sl::impairment_alignment) std::array<std::byte, 4096U> bytes{};
};

[[nodiscard]] sl::CwParams params() noexcept
{
    sl::CwParams result{};
    result.frequency_hz = 6000.0;
    result.ci_db = 0.0;
    result.random_phase = false;
    result.phase_degrees = 0.0;
    result.ramp_seconds = 0.0;
    result.fading.enabled = true;
    result.fading.spread = sl::units::FrequencyDelta{0.5, sl::units::Hz};
    return result;
}

[[nodiscard]] m110::Status configure_oracle(sl::WattersonPathState& state)
{
    sl::WattersonPath path{};
    path.fading = sl::PathFading::rayleigh;
    path.spread = sl::units::FrequencyDelta{0.5, sl::units::Hz};
    sl::WattersonPathReport report{};
    const sl::WattersonPathState::Setup setup{.path = &path, .power = 1.0, .index = stage_index, .generator = sl::det::Pcg32{seed, sl::cw_fading_rng_family, stage_index}, .observer = nullptr};
    return state.configure_rayleigh(setup, report);
}

[[nodiscard]] std::vector<float> render(const sl::CwParams& configuration, std::size_t frames, std::size_t block_size)
{
    StageStorage storage{};
    REQUIRE(sl::cw_size() <= storage.bytes.size());
    const sl::PlacedStage placed = sl::construct_cw(configuration, storage.bytes.data(), storage.bytes.size());
    REQUIRE(placed.has_value());
    sl::Impairment& stage = placed.value().get();
    REQUIRE(stage.prepare(sl::StageContext{.reference_rms = 1.0 / std::sqrt(2.0), .total_frames = frames, .seed = seed, .index = stage_index}).is_ok());
    std::vector<float> output(frames, 0.0F);

    for (std::size_t offset = 0U; offset < frames; offset += block_size)
    {
        const std::size_t count = std::min(block_size, frames - offset);
        REQUIRE(stage.process(output.data() + offset, count, offset, output.size()).has_value());
    }

    return output;
}

[[nodiscard]] float expected_sample(sl::ComplexFloat gain, std::uint64_t frame) noexcept
{
    const double angle = sl::det::two_pi * 6000.0 * static_cast<double>(frame) / 48000.0;
    return static_cast<float>(static_cast<double>(gain.real) * sl::det::sin(angle) + static_cast<double>(gain.imag) * sl::det::cos(angle));
}

} // namespace

TEST_CASE("CW fading validates finite Watterson spread bounds", "[cw][faded]")
{
    CHECK(sl::cw_fading_algorithm_version == 1U);
    CHECK(sl::cw_fading_rng_family == 12U);
    CHECK(sl::validate_cw_fading_params(params().fading).is_ok());
    sl::CwFadingParams invalid{.enabled = true, .spread = sl::units::FrequencyDelta{0.0, sl::units::Hz}};
    CHECK_FALSE(sl::validate_cw_fading_params(invalid).is_ok());
    invalid.spread = sl::units::FrequencyDelta{10.1, sl::units::Hz};
    CHECK_FALSE(sl::validate_cw_fading_params(invalid).is_ok());
}

TEST_CASE("CW fading matches one directly configured unit-power Watterson tap", "[cw][faded]")
{
    const sl::CwParams configuration = params();
    constexpr std::size_t frames = 12000U;
    const auto one_block = render(configuration, frames, frames);
    CHECK(render(configuration, frames, 1U) == one_block);
    CHECK(render(configuration, frames, 127U) == one_block);
    sl::WattersonPathState reference{};
    REQUIRE(configure_oracle(reference).is_ok());

    for (std::uint64_t frame = 0U; frame < one_block.size(); ++frame)
    {
        const sl::ComplexFloat gain = reference.next_gain(sl::units::SampleIndex{frame});
        CHECK(one_block[frame] == Catch::Approx(expected_sample(gain, frame)).margin(1.0e-6));
    }
}

TEST_CASE("CW fading consumes gain across pulse off time and starts at the CW window", "[cw][faded]")
{
    sl::CwParams configuration = params();
    configuration.pulse_period_frames = 8U;
    configuration.pulse_on_frames = 2U;
    configuration.window.start_seconds = 4.0 / 48000.0;
    configuration.window.has_duration = true;
    configuration.window.duration_seconds = 16.0 / 48000.0;
    const auto output = render(configuration, 20U, 3U);
    sl::WattersonPathState reference{};
    REQUIRE(configure_oracle(reference).is_ok());
    sl::ComplexFloat at_first_window{};
    sl::ComplexFloat at_next_pulse{};

    for (std::uint64_t frame = 0U; frame <= 8U; ++frame)
    {
        const sl::ComplexFloat gain = reference.next_gain(sl::units::SampleIndex{frame});

        if (frame == 0U)
        {
            at_first_window = gain;
        }

        if (frame == 8U)
        {
            at_next_pulse = gain;
        }
    }

    CHECK(output[4U] == Catch::Approx(expected_sample(at_first_window, 0U)).margin(1.0e-6));
    CHECK(output[6U] == 0.0F);
    CHECK(output[12U] == Catch::Approx(expected_sample(at_next_pulse, 8U)).margin(1.0e-6));
}

TEST_CASE("CW fading composes with sweep, FSK and Markov gates", "[cw][faded]")
{
    sl::CwParams configuration = params();
    configuration.frequency_hz = 0.0;
    configuration.sweep.enabled = true;
    configuration.sweep.low = sl::units::Frequency{3000.0, sl::units::Hz};
    configuration.sweep.high = sl::units::Frequency{9000.0, sl::units::Hz};
    configuration.sweep.slope = sl::FrequencySlope{1200.0};
    const auto sweep = render(configuration, 64U, 64U);
    CHECK(render(configuration, 64U, 1U) == sweep);
    CHECK(render(configuration, 64U, 7U) == sweep);
    configuration = params();
    configuration.fsk.enabled = true;
    configuration.fsk.baud = 12000.0;
    configuration.fsk.shift = sl::units::FrequencyDelta{2000.0, sl::units::Hz};
    const auto fsk = render(configuration, 64U, 64U);
    CHECK(render(configuration, 64U, 1U) == fsk);
    CHECK(render(configuration, 64U, 7U) == fsk);
    configuration = params();
    configuration.markov.enabled = true;
    configuration.markov.mean_on_seconds = 2.0 / 48000.0;
    configuration.markov.mean_off_seconds = 2.0 / 48000.0;
    const auto markov = render(configuration, 64U, 64U);
    CHECK(render(configuration, 64U, 1U) == markov);
    CHECK(render(configuration, 64U, 7U) == markov);
}

TEST_CASE("CW fading processing is contiguous and can retry after failed preparation", "[cw][faded]")
{
    StageStorage storage{};
    REQUIRE(sl::cw_size() <= storage.bytes.size());
    const sl::PlacedStage placed = sl::construct_cw(params(), storage.bytes.data(), storage.bytes.size());
    REQUIRE(placed.has_value());
    sl::Impairment& stage = placed.value().get();
    CHECK_FALSE(stage.prepare(sl::StageContext{.reference_rms = 0.0, .total_frames = 16U, .seed = seed, .index = stage_index}).is_ok());
    std::array<float, 8U> block{};
    CHECK_FALSE(stage.process(block.data(), 1U, 0U, block.size()).has_value());
    REQUIRE(stage.prepare(sl::StageContext{.reference_rms = 1.0 / std::sqrt(2.0), .total_frames = 16U, .seed = seed, .index = stage_index}).is_ok());
    CHECK(stage.process(nullptr, 0U, 0U, 0U).has_value());
    CHECK_FALSE(stage.process(nullptr, 1U, 0U, block.size()).has_value());
    CHECK_FALSE(stage.process(block.data(), 4U, 0U, 3U).has_value());
    CHECK_FALSE(stage.process(block.data(), 2U, UINT64_MAX, block.size()).has_value());
    CHECK_FALSE(stage.process(block.data(), 1U, 1U, block.size()).has_value());
    CHECK(stage.process(block.data(), block.size(), 0U, block.size()).has_value());
    CHECK_FALSE(stage.process(block.data(), 1U, 9U, block.size()).has_value());
}

TEST_CASE("CW fading configure, prepare and process do not allocate", "[cw][faded][allocation]")
{
    StageStorage storage{};
    REQUIRE(sl::cw_size() <= storage.bytes.size());
    std::array<float, 8U> block{};
    bool created = false;
    bool prepared = false;
    bool processed = false;
    std::uint64_t allocations = 0U;
    {
        const signal_lab_test::CountedRegion region;
        const sl::PlacedStage placed = sl::construct_cw(params(), storage.bytes.data(), storage.bytes.size());
        created = placed.has_value();

        if (created)
        {
            sl::Impairment& stage = placed.value().get();
            prepared = stage.prepare(sl::StageContext{.reference_rms = 1.0 / std::sqrt(2.0), .total_frames = 8U, .seed = seed, .index = stage_index}).is_ok();

            if (prepared)
            {
                processed = stage.process(block.data(), block.size(), 0U, block.size()).has_value();
            }
        }

        allocations = region.allocations();
    }

    CHECK(created);
    CHECK(prepared);
    CHECK(processed);
    CHECK(allocations == 0U);
}
