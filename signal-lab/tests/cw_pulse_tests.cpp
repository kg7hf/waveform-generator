// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

#include "signal_lab/det_math.hpp"
#include "signal_lab/impairment.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace sl = signal_lab;

namespace
{

struct StageStorage
{
    alignas(sl::impairment_alignment) std::array<std::byte, 4096U> bytes{};
};

[[nodiscard]] sl::CwParams pulse_params() noexcept
{
    sl::CwParams params{};
    params.frequency_hz = 12000.0;
    params.ci_db = 0.0;
    params.phase_degrees = 90.0;
    params.random_phase = false;
    params.ramp_seconds = 0.0;
    params.pulse_period_frames = 4U;
    params.pulse_on_frames = 2U;
    return params;
}

[[nodiscard]] std::vector<float> render(const sl::CwParams& params, std::uint64_t total_frames, std::size_t block_size, double reference_rms = 1.0)
{
    StageStorage storage{};
    REQUIRE(sl::cw_size() <= storage.bytes.size());
    const sl::PlacedStage placed = sl::construct_cw(params, storage.bytes.data(), storage.bytes.size());
    REQUIRE(placed.has_value());
    sl::Impairment& stage = placed.value().get();
    REQUIRE(stage.prepare(sl::StageContext{.reference_rms = reference_rms, .total_frames = total_frames}).is_ok());
    std::vector<float> result(static_cast<std::size_t>(total_frames), 0.0F);

    for (std::size_t offset = 0U; offset < result.size(); offset += block_size)
    {
        const std::size_t count = std::min(block_size, result.size() - offset);
        REQUIRE(stage.process(result.data() + offset, count, offset, result.size()).has_value());
    }

    return result;
}

} // namespace

TEST_CASE("CW pulse: independent literal sample vector", "[cw][pulse]")
{
    sl::CwParams params = pulse_params();
    params.frequency_hz = 6000.0;
    params.pulse_period_frames = 8U;
    params.pulse_on_frames = 4U;
    const std::array<float, 16U> expected{1.0F, 0.70710678F, 0.0F, -0.70710678F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.70710678F, 0.0F, -0.70710678F, 0.0F, 0.0F, 0.0F, 0.0F};
    const auto output = render(params, expected.size(), expected.size(), 1.0 / std::sqrt(2.0));

    for (std::size_t frame = 0U; frame < expected.size(); ++frame)
    {
        if (expected[frame] == 0.0F)
        {
            CHECK(output[frame] == Catch::Approx(0.0).margin(1.0e-6));
        }
        else
        {
            CHECK(output[frame] == expected[frame]);
        }
    }
}

TEST_CASE("CW pulse: exact sample mask, phase continuity and edge counts", "[cw][pulse]")
{
    const sl::CwParams params = pulse_params();
    const std::vector<float> output = render(params, 10U, 10U);
    const double amplitude = std::sqrt(2.0);
    const double radians_per_frame = sl::det::two_pi * params.frequency_hz / sl::sample_rate_hz;

    for (std::uint64_t frame = 0U; frame < output.size(); ++frame)
    {
        const bool on = frame % params.pulse_period_frames < params.pulse_on_frames;
        const double expected = on ? amplitude * std::sin(radians_per_frame * static_cast<double>(frame) + sl::det::half_pi) : 0.0;
        CHECK(output[frame] == Catch::Approx(expected).margin(1.0e-6));
    }

    StageStorage storage{};
    const sl::PlacedStage placed = sl::construct_cw(params, storage.bytes.data(), storage.bytes.size());
    REQUIRE(placed.has_value());
    sl::Impairment& stage = placed.value().get();
    REQUIRE(stage.prepare(sl::StageContext{.reference_rms = 1.0, .total_frames = 10U}).is_ok());
    std::array<float, 10U> block{};
    REQUIRE(stage.process(block.data(), block.size(), 0U, block.size()).has_value());
    CHECK(stage.stats().events_scheduled == 3U);
    CHECK(stage.stats().events_applied == 3U);
    CHECK(stage.stats().component_frames == 6U);
}

TEST_CASE("CW pulse: raised-cosine edges clamp to the actual final window fragment", "[cw][pulse]")
{
    sl::CwParams params = pulse_params();
    params.pulse_period_frames = 8U;
    params.pulse_on_frames = 6U;
    params.ramp_seconds = 4.0 / sl::sample_rate_hz; // four samples; clamp at half the actual on length.
    params.frequency_hz = 6000.0;
    params.window.has_duration = true;
    params.window.duration_seconds = 11.0 / sl::sample_rate_hz; // final pulse has only three samples.
    params.window.start_seconds = 2.0 / sl::sample_rate_hz;     // pulse phase is relative to the window start.
    const std::vector<float> output = render(params, 13U, 13U);
    const double amplitude = std::sqrt(2.0);
    const double step = sl::det::two_pi * params.frequency_hz / sl::sample_rate_hz;
    CHECK(output[0] == 0.0F);
    CHECK(output[1] == 0.0F);
    CHECK(output[2] == Catch::Approx(0.0).margin(1.0e-6));
    CHECK(output[3] == Catch::Approx(amplitude * std::sin(step + sl::det::half_pi) * 0.25).margin(1.0e-6));
    CHECK(output[4] == Catch::Approx(amplitude * std::sin(2.0 * step + sl::det::half_pi) * 0.75).margin(1.0e-6));
    CHECK(output[5] == Catch::Approx(amplitude * std::sin(3.0 * step + sl::det::half_pi) * 0.75).margin(1.0e-6));
    CHECK(output[6] == Catch::Approx(amplitude * std::sin(4.0 * step + sl::det::half_pi) * 0.25).margin(1.0e-6));
    CHECK(output[7] == Catch::Approx(0.0).margin(1.0e-6));
    CHECK(output[8] == 0.0F);
    CHECK(output[9] == 0.0F);
    CHECK(output[10] == Catch::Approx(0.0).margin(1.0e-6));
    CHECK(output[11] == Catch::Approx(amplitude * std::sin(9.0 * step + sl::det::half_pi)).margin(1.0e-6));
    CHECK(output[12] == Catch::Approx(0.0).margin(1.0e-6));
}

TEST_CASE("CW pulse: output is bit-identical across partitions and uses 64-bit frame phase", "[cw][pulse]")
{
    sl::CwParams params = pulse_params();
    params.pulse_period_frames = 997U;
    params.pulse_on_frames = 611U;
    params.ramp_seconds = 0.001;
    const auto reference = render(params, 5000U, 5000U);

    for (const std::size_t partition : {1U, 63U, 257U, 1024U})
    {
        CHECK(render(params, 5000U, partition) == reference);
    }

    StageStorage storage{};
    auto late_params = pulse_params();
    late_params.frequency_hz = 1000.0;
    const sl::PlacedStage placed = sl::construct_cw(late_params, storage.bytes.data(), storage.bytes.size());
    REQUIRE(placed.has_value());
    sl::Impairment& stage = placed.value().get();
    REQUIRE(stage.prepare(sl::StageContext{.reference_rms = 1.0, .total_frames = 0U}).is_ok());
    std::array<float, 3U> late{};
    constexpr std::uint64_t late_start = (std::uint64_t{1U} << 32U) + 4U;
    REQUIRE(stage.process(late.data(), late.size(), late_start, late.size()).has_value());
    // 2^32+4 is a pulse start; modulo the independent 48-frame carrier
    // period it is frame 20, not frame 4 as a truncated uint32 cursor would be.
    CHECK(late[0] == Catch::Approx(-1.224744871391589).margin(2.0e-6));
    CHECK(late[1] == Catch::Approx(-1.306562964876377).margin(2.0e-6));
    CHECK(late[2] == 0.0F);
}

TEST_CASE("CW pulse: C/I calibrates un-ramped on-state tone RMS within 0.1 dB", "[cw][pulse]")
{
    sl::CwParams params{};
    params.frequency_hz = 1000.0;
    params.ci_db = 12.0;
    params.phase_degrees = 17.0;
    params.random_phase = false;
    params.ramp_seconds = 0.0;
    params.pulse_period_frames = 9600U;
    params.pulse_on_frames = 4800U;
    const auto output = render(params, 4800U, 257U);
    double energy = 0.0;

    for (const float sample : output)
    {
        energy += static_cast<double>(sample) * sample;
    }

    const double measured_rms = std::sqrt(energy / static_cast<double>(output.size()));
    const double measured_ci = -20.0 * std::log10(measured_rms);
    CHECK(std::abs(measured_ci - params.ci_db) <= 0.1);
}

TEST_CASE("CW pulse: disabled settings retain legacy output and malformed typed schedules refuse", "[cw][pulse]")
{
    sl::CwParams legacy{};
    legacy.frequency_hz = 12000.0;
    legacy.ci_db = 0.0;
    legacy.phase_degrees = 90.0;
    legacy.random_phase = false;
    legacy.ramp_seconds = 0.0;
    const auto output = render(legacy, 4U, 4U);
    const double step = sl::det::two_pi * legacy.frequency_hz / sl::sample_rate_hz;

    for (std::uint64_t frame = 0U; frame < output.size(); ++frame)
    {
        CHECK(output[frame] == Catch::Approx(std::sqrt(2.0) * std::sin(step * frame + sl::det::half_pi)).margin(1.0e-6));
    }

    for (const auto& [period, on] : {std::pair<std::uint64_t, std::uint64_t>{0U, 4U}, {4U, 0U}, {4U, 5U}, {sl::max_cw_pulse_frames + 1U, 1U}})
    {
        sl::CwParams invalid = pulse_params();
        invalid.pulse_period_frames = period;
        invalid.pulse_on_frames = on;
        StageStorage storage{};
        const sl::PlacedStage placed = sl::construct_cw(invalid, storage.bytes.data(), storage.bytes.size());
        REQUIRE(placed.has_value());
        CHECK_FALSE(placed.value().get().prepare(sl::StageContext{.reference_rms = 1.0, .total_frames = 10U}).is_ok());
    }
}
