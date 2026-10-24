// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

#include "allocation_counter.hpp"
#include "signal_lab/cw_fsk.hpp"
#include "signal_lab/impairment.hpp"
#include "signal_lab/markov_gate.hpp"
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

[[nodiscard]] sl::FskParams fsk_params(double baud = 12000.0) noexcept
{
    return sl::FskParams{.enabled = true, .baud = baud, .shift = sl::units::FrequencyDelta{6000.0, sl::units::Hz}, .pattern = sl::FskPattern::alternating};
}

[[nodiscard]] sl::CwParams cw_params(double baud = 12000.0) noexcept
{
    sl::CwParams params{};
    params.frequency_hz = 6000.0;
    params.ci_db = 0.0;
    params.random_phase = false;
    params.phase_degrees = 90.0;
    params.ramp_seconds = 0.0;
    params.fsk = fsk_params(baud);
    return params;
}

[[nodiscard]] std::vector<float> render(const sl::CwParams& params, std::size_t frames, std::size_t block_size, double reference_rms = 1.0 / std::sqrt(2.0))
{
    StageStorage storage{};
    REQUIRE(sl::cw_size() <= storage.bytes.size());
    const sl::PlacedStage placed = sl::construct_cw(params, storage.bytes.data(), storage.bytes.size());
    REQUIRE(placed.has_value());
    sl::Impairment& stage = placed.value().get();
    REQUIRE(stage.prepare(sl::StageContext{.reference_rms = reference_rms, .total_frames = frames, .seed = 123U, .index = 7U}).is_ok());
    std::vector<float> output(frames, 0.0F);

    for (std::size_t offset = 0U; offset < frames; offset += block_size)
    {
        const std::size_t count = std::min(block_size, frames - offset);
        REQUIRE(stage.process(output.data() + offset, count, offset, output.size()).has_value());
    }

    return output;
}

} // namespace

TEST_CASE("CW FSK: exact integer sample periods and strict baud refusal", "[cw][fsk]")
{
    CHECK(sl::cw_fsk_algorithm_version == 1U);
    CHECK(sl::cw_fsk_rng_family == 11U);
    CHECK(sl::cw_fsk_symbol_frames(fsk_params(37.5)).value() == 1280U);
    CHECK(sl::cw_fsk_symbol_frames(fsk_params(48000.0)).value() == 1U);
    CHECK(sl::cw_fsk_symbol_frames(fsk_params(1.0e-9)).value() == sl::cw_fsk_max_symbol_frames);

    for (const double baud : {0.0, -1.0, 110.0, 48001.0, 0.9e-9, std::numeric_limits<double>::infinity()})
    {
        CHECK_FALSE(sl::cw_fsk_symbol_frames(fsk_params(baud)).has_value());
    }
}

TEST_CASE("CW FSK: full separation gives bounded mark and space frequencies", "[cw][fsk]")
{
    sl::FskParams params = fsk_params();
    CHECK(sl::validate_cw_fsk_params(params, 6000.0).is_ok()); // 9000 Hz mark, 3000 Hz space.
    params.shift = sl::units::FrequencyDelta{12000.0, sl::units::Hz};
    CHECK_FALSE(sl::validate_cw_fsk_params(params, 6000.0).is_ok());
    params.shift = sl::units::FrequencyDelta{0.0, sl::units::Hz};
    CHECK_FALSE(sl::validate_cw_fsk_params(params, 6000.0).is_ok());
    params.shift = sl::units::FrequencyDelta{1.0e-300, sl::units::Hz};
    CHECK_FALSE(sl::validate_cw_fsk_params(params, 6000.0).is_ok()); // Distinct tones must survive rounding.
    params = fsk_params();
    params.pattern = static_cast<sl::FskPattern>(255U);
    CHECK_FALSE(sl::validate_cw_fsk_params(params, 6000.0).is_ok());
    CHECK_FALSE(sl::validate_cw_fsk_params(fsk_params(), std::numeric_limits<double>::quiet_NaN()).is_ok());
    params = fsk_params();
    params.shift = sl::units::FrequencyDelta{5.0e-16, sl::units::Hz};
    // Distinct binary64 frequencies can still round to the same Q0.64 step.
    CHECK_FALSE(sl::validate_cw_fsk_params(params, 2.0).is_ok());
}

TEST_CASE("CW FSK: literal alternating PCM vector and exact symbol boundary", "[cw][fsk]")
{
    const std::array<float, 8U> expected{1.0F, 0.38268343F, -0.70710678F, -0.92387953F, 0.0F, 0.38268343F, 0.70710678F, 0.92387953F};
    const auto output = render(cw_params(), expected.size(), expected.size());

    for (std::size_t index = 0U; index < expected.size(); ++index)
    {
        CHECK(output[index] == Catch::Approx(expected[index]).margin(1.0e-6));
    }
}

TEST_CASE("CW FSK: random symbols use the family-11 first-draw rule", "[cw][fsk]")
{
    sl::CwParams params = cw_params();
    params.fsk.pattern = sl::FskPattern::random;
    const auto output = render(params, 9U, 9U);
    // seed 123/index 7/family 11 has first uniform < 0.5 (SPACE), then >= 0.5 (MARK).
    CHECK(output[3U] == Catch::Approx(0.38268343F).margin(1.0e-6));
    CHECK(output[4U] == Catch::Approx(0.0F).margin(1.0e-6));
    CHECK(output[5U] == Catch::Approx(-0.92387953F).margin(1.0e-6));

    for (const std::size_t block_size : {1U, 3U, 127U})
    {
        CHECK(render(params, 9U, block_size) == output);
    }
}

TEST_CASE("CW FSK: tone phase continues through periodic and Markov OFF gates", "[cw][fsk]")
{
    sl::CwParams params = cw_params();
    params.pulse_period_frames = 6U;
    params.pulse_on_frames = 2U;
    const auto pulsed = render(params, 7U, 7U);
    CHECK(pulsed[2U] == 0.0F);
    CHECK(pulsed[5U] == 0.0F);
    CHECK(pulsed[6U] == Catch::Approx(0.70710678F).margin(1.0e-6));
    params = cw_params();
    params.markov.enabled = true;
    params.markov.mean_on_seconds = 1.0 / sl::sample_rate_hz;
    params.markov.mean_off_seconds = 1.0 / sl::sample_rate_hz;
    const auto markov = render(params, 5U, 5U);
    CHECK(markov[0U] == Catch::Approx(1.0F).margin(1.0e-6));
    CHECK(markov[1U] == 0.0F);
    CHECK(markov[2U] == Catch::Approx(-0.70710678F).margin(1.0e-6));
}

TEST_CASE("CW FSK: window-relative symbol zero and deterministic partitions", "[cw][fsk]")
{
    sl::CwParams params = cw_params(37.5);
    params.window.start_seconds = 2.0 / sl::sample_rate_hz;
    const auto expected = render(params, 1300U, 1300U);
    CHECK(expected[0U] == 0.0F);
    CHECK(expected[1U] == 0.0F);
    CHECK(expected[2U] == Catch::Approx(1.0F).margin(1.0e-6));
    CHECK(expected[1281U] == Catch::Approx(0.38268343F).margin(1.0e-6)); // last MARK sample.
    CHECK(expected[1282U] == Catch::Approx(1.0F).margin(1.0e-6));        // first SPACE sample at 2+1280.
    CHECK(expected[1283U] == Catch::Approx(0.92387953F).margin(1.0e-6));

    for (const std::size_t block_size : {1U, 127U, 512U})
    {
        CHECK(render(params, 1300U, block_size) == expected);
    }
}

TEST_CASE("CW FSK: incompatible modes and failed preparation refuse safely", "[cw][fsk]")
{
    sl::CwParams params = cw_params();
    params.sweep.enabled = true;
    params.sweep.low = sl::units::Frequency{1000.0, sl::units::Hz};
    params.sweep.high = sl::units::Frequency{2000.0, sl::units::Hz};
    params.sweep.slope = sl::FrequencySlope{12000.0};
    params.frequency_hz = 0.0;
    StageStorage storage{};
    const sl::PlacedStage conflict = sl::construct_cw(params, storage.bytes.data(), storage.bytes.size());
    REQUIRE(conflict.has_value());
    CHECK_FALSE(conflict.value().get().prepare(sl::StageContext{.reference_rms = 1.0, .total_frames = 10U}).is_ok());
    params = cw_params();
    StageStorage retry_storage{};
    const sl::PlacedStage retry_placed = sl::construct_cw(params, retry_storage.bytes.data(), retry_storage.bytes.size());
    REQUIRE(retry_placed.has_value());
    sl::Impairment& stage = retry_placed.value().get();
    CHECK_FALSE(stage.prepare(sl::StageContext{.reference_rms = 0.0, .total_frames = 10U}).is_ok());
    std::array<float, 8U> block{};
    CHECK_FALSE(stage.process(block.data(), 1U, 0U, block.size()).has_value());
    CHECK(stage.prepare(sl::StageContext{.reference_rms = 1.0 / std::sqrt(2.0), .total_frames = 10U}).is_ok());
    CHECK_FALSE(stage.process(block.data(), 1U, 1U, block.size()).has_value());
    CHECK(stage.process(block.data(), 1U, 0U, block.size()).has_value());
    CHECK_FALSE(stage.process(block.data(), 3U, 1U, 2U).has_value());
    CHECK_FALSE(stage.process(block.data(), 2U, UINT64_MAX, 2U).has_value());
}

TEST_CASE("CW FSK: processing does not allocate", "[cw][fsk][allocation]")
{
    const sl::CwParams params = cw_params();
    StageStorage storage{};
    REQUIRE(sl::cw_size() <= storage.bytes.size());
    const sl::PlacedStage placed = sl::construct_cw(params, storage.bytes.data(), storage.bytes.size());
    REQUIRE(placed.has_value());
    sl::Impairment& stage = placed.value().get();
    REQUIRE(stage.prepare(sl::StageContext{.reference_rms = 1.0 / std::sqrt(2.0), .total_frames = 128U}).is_ok());
    std::array<float, 128U> block{};
    std::uint64_t allocations = 0U;
    bool processed = false;
    {
        const signal_lab_test::CountedRegion region;
        processed = stage.process(block.data(), block.size(), 0U, block.size()).has_value();
        allocations = region.allocations();
    }

    CHECK(processed);
    CHECK(allocations == 0U);
}
