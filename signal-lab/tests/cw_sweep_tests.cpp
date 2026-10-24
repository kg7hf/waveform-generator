// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

#include "allocation_counter.hpp"
#include "signal_lab/cw_sweep.hpp"
#include "signal_lab/det_math.hpp"
#include "signal_lab/impairment.hpp"
#include "signal_lab/units/frequency.hpp"
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace sl = signal_lab;

namespace
{

struct StageStorage
{
    alignas(sl::impairment_alignment) std::array<std::byte, 4096U> bytes{};
};

[[nodiscard]] sl::CwParams sweep_params() noexcept
{
    sl::CwParams params{};
    params.sweep.enabled = true;
    params.sweep.low = sl::units::Frequency{1000.0, sl::units::Hz};
    params.sweep.high = sl::units::Frequency{2000.0, sl::units::Hz};
    params.sweep.slope = sl::FrequencySlope{12000.0};
    params.frequency_hz = 0.0;
    params.ci_db = 0.0;
    params.phase_degrees = 90.0;
    params.random_phase = false;
    params.ramp_seconds = 0.0;
    return params;
}

[[nodiscard]] std::vector<float> render(const sl::CwParams& params, std::size_t frames, std::size_t block_size)
{
    StageStorage storage{};
    REQUIRE(sl::cw_size() <= storage.bytes.size());
    const sl::PlacedStage placed = sl::construct_cw(params, storage.bytes.data(), storage.bytes.size());
    REQUIRE(placed.has_value());
    sl::Impairment& stage = placed.value().get();
    REQUIRE(stage.prepare(sl::StageContext{.reference_rms = 1.0, .total_frames = frames}).is_ok());
    std::vector<float> output(frames, 0.0F);

    for (std::size_t offset = 0U; offset < frames; offset += block_size)
    {
        const std::size_t count = std::min(block_size, frames - offset);
        REQUIRE(stage.process(output.data() + offset, count, offset, output.size()).has_value());
    }

    return output;
}

} // namespace

TEST_CASE("CW sweep: endpoints, triangle turnarounds and wide frame index", "[cw][sweep]")
{
    const sl::CwParams params = sweep_params();
    const auto& sweep = params.sweep;
    REQUIRE(sl::validate_cw_sweep_params(sweep).is_ok());
    CHECK(sl::cw_sweep_algorithm_version == 1U);
    CHECK(sl::cw_sweep_cycle_step(sweep) != 0U);
    CHECK(sl::instantaneous_cw_frequency(sweep, sl::units::SampleIndex{0U}).in(sl::units::Hz) == Catch::Approx(1000.0));
    CHECK(sl::instantaneous_cw_frequency(sweep, sl::units::SampleIndex{4000U}).in(sl::units::Hz) == Catch::Approx(2000.0).margin(0.01));
    CHECK(sl::instantaneous_cw_frequency(sweep, sl::units::SampleIndex{8000U}).in(sl::units::Hz) == Catch::Approx(1000.0).margin(0.01));
    CHECK(sl::instantaneous_cw_frequency(sweep, sl::units::SampleIndex{12000U}).in(sl::units::Hz) == Catch::Approx(2000.0).margin(0.01));
    constexpr std::uint64_t frame = (std::uint64_t{1U} << 40U) + 12345U;
    const double phase = std::fmod(static_cast<double>(frame) * 12000.0 / (2.0 * 1000.0 * sl::sample_rate_hz), 1.0);
    const double triangle = phase < 0.5 ? 1000.0 + 2000.0 * phase : 1000.0 + 2000.0 * (1.0 - phase);
    CHECK(sl::instantaneous_cw_frequency(sweep, sl::units::SampleIndex{frame}).in(sl::units::Hz) == Catch::Approx(triangle).margin(0.01));
    const double crossing_average = sl::interval_average_cw_frequency(sweep, (std::uint64_t{3U} << 61U), (std::uint64_t{1U} << 62U));
    CHECK(crossing_average == Catch::Approx(1875.0).margin(1.0e-9));
}

TEST_CASE("CW sweep: malformed numeric ranges and unrepresentable slopes refuse", "[cw][sweep]")
{
    sl::CwSweepParams invalid = sweep_params().sweep;
    invalid.low = sl::units::Frequency{0.0, sl::units::Hz};
    CHECK_FALSE(sl::validate_cw_sweep_params(invalid).is_ok());
    invalid = sweep_params().sweep;
    invalid.high = invalid.low;
    CHECK_FALSE(sl::validate_cw_sweep_params(invalid).is_ok());
    invalid = sweep_params().sweep;
    invalid.high = sl::units::Frequency{24000.0, sl::units::Hz};
    CHECK_FALSE(sl::validate_cw_sweep_params(invalid).is_ok());
    invalid = sweep_params().sweep;
    invalid.slope = sl::FrequencySlope{0.0};
    CHECK_FALSE(sl::validate_cw_sweep_params(invalid).is_ok());
    invalid.slope = sl::FrequencySlope{1.0e-300};
    CHECK_FALSE(sl::validate_cw_sweep_params(invalid).is_ok());
    invalid.slope = sl::FrequencySlope{1.0e300};
    CHECK_FALSE(sl::validate_cw_sweep_params(invalid).is_ok());
    sl::CwParams direct = sweep_params();
    direct.phase_degrees = std::numeric_limits<double>::quiet_NaN();
    StageStorage storage{};
    const sl::PlacedStage placed = sl::construct_cw(direct, storage.bytes.data(), storage.bytes.size());
    REQUIRE(placed.has_value());
    sl::Impairment& stage = placed.value().get();
    CHECK_FALSE(stage.prepare(sl::StageContext{.reference_rms = 1.0, .total_frames = 4U}).is_ok());
    std::array<float, 1U> block{};
    CHECK_FALSE(stage.process(block.data(), block.size(), 0U, block.size()).has_value());
    direct.phase_degrees = 90.0;
    StageStorage valid_storage{};
    const sl::PlacedStage valid = sl::construct_cw(direct, valid_storage.bytes.data(), valid_storage.bytes.size());
    REQUIRE(valid.has_value());
    sl::Impairment& retry = valid.value().get();
    CHECK_FALSE(retry.prepare(sl::StageContext{.reference_rms = 0.0, .total_frames = 4U}).is_ok());
    CHECK(retry.prepare(sl::StageContext{.reference_rms = 1.0, .total_frames = 4U}).is_ok());
}

TEST_CASE("CW sweep: independent turnaround samples and exact block partitions", "[cw][sweep]")
{
    const sl::CwParams params = sweep_params();
    const auto expected = render(params, 8003U, 8003U);
    const double peak = std::sqrt(2.0);
    const double turn_cycles = 125.0;
    const double before_turn = peak * std::sin(sl::det::two_pi * (turn_cycles - (2000.0 - 0.125) / sl::sample_rate_hz) + sl::det::half_pi);
    const double at_turn = peak * std::sin(sl::det::two_pi * turn_cycles + sl::det::half_pi);
    const double after_turn = peak * std::sin(sl::det::two_pi * (turn_cycles + (2000.0 - 0.125) / sl::sample_rate_hz) + sl::det::half_pi);
    CHECK(expected[3999U] == Catch::Approx(before_turn).margin(2.0e-5));
    CHECK(expected[4000U] == Catch::Approx(at_turn).margin(2.0e-5));
    CHECK(expected[4001U] == Catch::Approx(after_turn).margin(2.0e-5));

    for (const std::size_t block_size : {1U, 127U, 2048U})
    {
        CHECK(render(params, 8003U, block_size) == expected);
    }
}

TEST_CASE("CW sweep: pulse gaps keep carrier phase continuous and noncontiguous input refuses", "[cw][sweep]")
{
    sl::CwParams params = sweep_params();
    params.pulse_period_frames = 600U;
    params.pulse_on_frames = 300U;
    const auto output = render(params, 603U, 603U);
    const double integrated_cycles = (1000.0 * 600.0 + 0.125 * 600.0 * 600.0) / sl::sample_rate_hz;
    const double expected_restart = std::sqrt(2.0) * std::sin(sl::det::two_pi * integrated_cycles + sl::det::half_pi);
    CHECK(output[599U] == 0.0F);
    CHECK(output[600U] == Catch::Approx(expected_restart).margin(2.0e-5));
    StageStorage storage{};
    const sl::PlacedStage placed = sl::construct_cw(params, storage.bytes.data(), storage.bytes.size());
    REQUIRE(placed.has_value());
    sl::Impairment& stage = placed.value().get();
    REQUIRE(stage.prepare(sl::StageContext{.reference_rms = 1.0, .total_frames = 0U}).is_ok());
    std::array<float, 2U> block{};
    CHECK_FALSE(stage.process(block.data(), block.size(), 1U, block.size()).has_value());
    REQUIRE(stage.process(block.data(), block.size(), 0U, block.size()).has_value());
    CHECK_FALSE(stage.process(block.data(), block.size(), 3U, block.size()).has_value());
    CHECK_FALSE(stage.process(block.data(), block.size(), 2U, 1U).has_value());
    CHECK_FALSE(stage.process(block.data(), block.size(), UINT64_MAX, block.size()).has_value());
    CHECK_FALSE(stage.process(nullptr, block.size(), 2U, block.size()).has_value());
    REQUIRE(stage.process(nullptr, 0U, UINT64_MAX, 0U).has_value());
    block.fill(0.0F);
    REQUIRE(stage.process(block.data(), block.size(), 2U, block.size()).has_value());
    const auto uninterrupted = render(params, 4U, 4U);
    CHECK(block[0] == uninterrupted[2]);
    CHECK(block[1] == uninterrupted[3]);
}

TEST_CASE("CW sweep: literal samples anchor frequency integration", "[cw][sweep]")
{
    const std::array<float, 8U> expected{1.41421356F, 1.40211175F, 1.36600144F, 1.30648325F, 1.22455971F, 1.12161879F, 0.99941078F, 0.86001884F};
    const auto output = render(sweep_params(), expected.size(), expected.size());

    for (std::size_t index = 0U; index < expected.size(); ++index)
    {
        CHECK(output[index] == Catch::Approx(expected[index]).margin(1.0e-6));
    }
}

TEST_CASE("CW sweep: on-state C/I remains calibrated", "[cw][sweep]")
{
    sl::CwParams params = sweep_params();
    params.sweep.low = sl::units::Frequency{300.0, sl::units::Hz};
    params.sweep.high = sl::units::Frequency{3300.0, sl::units::Hz};
    params.sweep.slope = sl::FrequencySlope{300.0};
    params.ci_db = 12.0;
    const auto output = render(params, 48000U, 127U);
    double energy = 0.0;

    for (const float sample : output)
    {
        energy += static_cast<double>(sample) * sample;
    }

    const double measured_ci = -10.0 * std::log10(energy / static_cast<double>(output.size()));
    CHECK(std::abs(measured_ci - params.ci_db) <= 0.1);
}

TEST_CASE("CW sweep: oscillator processing does not allocate", "[cw][sweep][allocation]")
{
    const sl::CwParams params = sweep_params();
    StageStorage storage{};
    REQUIRE(sl::cw_size() <= storage.bytes.size());
    const sl::PlacedStage placed = sl::construct_cw(params, storage.bytes.data(), storage.bytes.size());
    REQUIRE(placed.has_value());
    sl::Impairment& stage = placed.value().get();
    REQUIRE(stage.prepare(sl::StageContext{.reference_rms = 1.0, .total_frames = 128U}).is_ok());
    std::array<float, 128U> block{};
    bool processed = false;
    std::uint64_t allocations = 0U;
    {
        const signal_lab_test::CountedRegion region;
        processed = stage.process(block.data(), block.size(), 0U, block.size()).has_value();
        allocations = region.allocations();
    }

    CHECK(processed);
    CHECK(allocations == 0U);
}
