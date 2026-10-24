// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

#include "allocation_counter.hpp"
#include "signal_lab/cw_sweep.hpp"
#include "signal_lab/impairment.hpp"
#include "signal_lab/markov_gate.hpp"

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

[[nodiscard]] sl::MarkovGateParams markov_params(double on_frames, double off_frames) noexcept
{
    return sl::MarkovGateParams{.enabled = true, .mean_on_seconds = on_frames / static_cast<double>(sl::sample_rate_hz), .mean_off_seconds = off_frames / static_cast<double>(sl::sample_rate_hz)};
}

[[nodiscard]] sl::CwParams cw_params(double on_frames, double off_frames) noexcept
{
    sl::CwParams params{};
    params.frequency_hz = 6000.0;
    params.ci_db = 0.0;
    params.random_phase = false;
    params.phase_degrees = 90.0;
    params.ramp_seconds = 0.0;
    params.markov = markov_params(on_frames, off_frames);
    return params;
}

[[nodiscard]] std::vector<float> render(const sl::CwParams& params, std::size_t frames, std::size_t block_size)
{
    StageStorage storage{};
    REQUIRE(sl::cw_size() <= storage.bytes.size());
    const sl::PlacedStage placed = sl::construct_cw(params, storage.bytes.data(), storage.bytes.size());
    REQUIRE(placed.has_value());
    sl::Impairment& stage = placed.value().get();
    REQUIRE(stage.prepare(sl::StageContext{.reference_rms = 1.0, .total_frames = frames, .seed = 123U, .index = 7U}).is_ok());
    std::vector<float> output(frames, 0.0F);

    for (std::size_t offset = 0U; offset < frames; offset += block_size)
    {
        const std::size_t count = std::min(block_size, frames - offset);
        REQUIRE(stage.process(output.data() + offset, count, offset, output.size()).has_value());
    }

    return output;
}

} // namespace

TEST_CASE("Markov gate: independent initial geometric dwell sequence", "[markov]")
{
    sl::MarkovGate gate{};
    REQUIRE(gate.configure(markov_params(4.0, 3.0), 123U, 7U).is_ok());
    constexpr std::array<std::uint64_t, 8U> expected_dwell{3U, 2U, 4U, 1U, 3U, 2U, 4U, 3U};
    bool state_on = true;

    for (const std::uint64_t dwell : expected_dwell)
    {
        CHECK(gate.on() == state_on);
        CHECK(gate.dwell_frames() == dwell);
        bool transition_to_on = false;

        for (std::uint64_t frame = 0U; frame < dwell; ++frame)
        {
            transition_to_on = gate.advance();
        }

        state_on = !state_on;
        CHECK(gate.on() == state_on);
        CHECK(transition_to_on == state_on);
    }
}

TEST_CASE("Markov gate: long-run dwell means approach configured frame means", "[markov]")
{
    constexpr std::uint64_t draws = 10000U;
    sl::MarkovGate gate{};
    REQUIRE(gate.configure(markov_params(4.0, 7.0), 0xA531U, 2U).is_ok());
    double on_sum = 0.0;
    double off_sum = 0.0;
    std::uint64_t on_count = 0U;
    std::uint64_t off_count = 0U;

    for (std::uint64_t draw = 0U; draw < draws; ++draw)
    {
        const std::uint64_t dwell = gate.dwell_frames();

        if (gate.on())
        {
            on_sum += static_cast<double>(dwell);
            ++on_count;
        }
        else
        {
            off_sum += static_cast<double>(dwell);
            ++off_count;
        }

        for (std::uint64_t frame = 0U; frame < dwell; ++frame)
        {
            (void)gate.advance();
        }
    }

    CHECK(on_count > 0U);
    CHECK(off_count > 0U);
    CHECK(on_sum / static_cast<double>(on_count) == Catch::Approx(4.0).margin(0.15));
    CHECK(off_sum / static_cast<double>(off_count) == Catch::Approx(7.0).margin(0.25));
}

TEST_CASE("Markov gate: deterministic partitions and reconfiguration reset", "[markov]")
{
    sl::MarkovGate first{};
    sl::MarkovGate second{};
    REQUIRE(first.configure(markov_params(13.0, 9.0), 81U, 4U).is_ok());
    REQUIRE(second.configure(markov_params(13.0, 9.0), 81U, 4U).is_ok());
    const std::uint64_t initial_dwell = second.dwell_frames();
    std::array<bool, 500U> one_by_one{};

    for (bool& on : one_by_one)
    {
        on = first.on();
        (void)first.advance();
    }

    std::array<bool, 500U> chunks{};
    std::size_t offset = 0U;

    for (const std::size_t count : {17U, 1U, 64U, 127U, 291U})
    {
        for (std::size_t index = 0U; index < count; ++index)
        {
            chunks[offset++] = second.on();
            (void)second.advance();
        }
    }

    CHECK(chunks == one_by_one);
    REQUIRE(second.configure(markov_params(13.0, 9.0), 81U, 4U).is_ok());
    CHECK(second.on() == one_by_one[0]);
    CHECK(second.dwell_frames() == initial_dwell);
}

TEST_CASE("Markov gate: one-frame means still consume draws and invalid means refuse", "[markov]")
{
    CHECK(sl::validate_markov_gate_params(markov_params(1.0, 1.0)).is_ok());
    sl::MarkovGate gate{};
    REQUIRE(gate.configure(markov_params(1.0, 1.0), 5U, 0U).is_ok());
    CHECK(gate.on());
    CHECK(gate.dwell_frames() == 1U);
    CHECK_FALSE(gate.advance());
    CHECK_FALSE(gate.on());
    CHECK(gate.dwell_frames() == 1U);

    for (const sl::MarkovGateParams invalid : {markov_params(0.0, 4.0), markov_params(0.5, 4.0), markov_params(4.0, sl::markov_gate_max_mean_frames + 1.0),
                                               sl::MarkovGateParams{.enabled = true, .mean_on_seconds = std::numeric_limits<double>::infinity(), .mean_off_seconds = 1.0}})
    {
        CHECK_FALSE(sl::validate_markov_gate_params(invalid).is_ok());
        CHECK_FALSE(gate.configure(invalid, 5U, 0U).is_ok());
    }
}

TEST_CASE("CW Markov gate: ON/OFF phases, ramps, sweep continuity and partition identity", "[cw][markov]")
{
    // The literal family-10 vector above starts with ON=3, OFF=2 at means 4/3.
    sl::CwParams params = cw_params(4.0, 3.0);
    params.ramp_seconds = 2.0 / sl::sample_rate_hz;
    const auto reference = render(params, 80U, 80U);

    for (const std::size_t block_size : {1U, 7U, 31U})
    {
        CHECK(render(params, 80U, block_size) == reference);
    }

    CHECK(reference[0U] == 0.0F); // first ON-dwell edge ramp
    CHECK(reference[1U] == Catch::Approx(1.0).margin(1.0e-6));
    CHECK(reference[2U] == Catch::Approx(0.0).margin(1.0e-6));
    CHECK(reference[3U] == 0.0F); // first OFF dwell
    params.sweep.enabled = true;
    params.sweep.low = sl::units::Frequency{1000.0, sl::units::Hz};
    params.sweep.high = sl::units::Frequency{2000.0, sl::units::Hz};
    params.sweep.slope = sl::FrequencySlope{12000.0};
    params.frequency_hz = 0.0;
    const auto swept = render(params, 80U, 13U);
    CHECK(swept[0U] == 0.0F);
    CHECK(swept[3U] == 0.0F);
    params = cw_params(3.0, 2.0);
    params.ramp_seconds = 2.0 / sl::sample_rate_hz;
    params.window.has_duration = true;
    params.window.duration_seconds = 2.0 / sl::sample_rate_hz;
    const auto truncated = render(params, 2U, 1U);
    CHECK(truncated[0U] == 0.0F);
    CHECK(truncated[1U] == Catch::Approx(0.0).margin(1.0e-6));
}

TEST_CASE("CW Markov gate: strict sequencing, mutually exclusive schedules and no allocation", "[cw][markov][allocation]")
{
    sl::CwParams params = cw_params(3.0, 2.0);
    StageStorage storage{};
    REQUIRE(sl::cw_size() <= storage.bytes.size());
    const sl::PlacedStage placed = sl::construct_cw(params, storage.bytes.data(), storage.bytes.size());
    REQUIRE(placed.has_value());
    sl::Impairment& stage = placed.value().get();
    REQUIRE(stage.prepare(sl::StageContext{.reference_rms = 1.0, .total_frames = 20U, .seed = 123U, .index = 7U}).is_ok());
    std::array<float, 8U> block{};
    CHECK_FALSE(stage.process(block.data(), 4U, 1U, 4U).has_value());
    CHECK_FALSE(stage.process(block.data(), 4U, UINT64_MAX, 8U).has_value());
    std::uint64_t allocations = 0U;
    bool processed = false;
    {
        const signal_lab_test::CountedRegion region;
        processed = stage.process(block.data(), 4U, 0U, 4U).has_value();
        allocations = region.allocations();
    }

    CHECK(processed);
    CHECK(allocations == 0U);
    CHECK_FALSE(stage.process(block.data(), 5U, 4U, 4U).has_value());
    CHECK(stage.process(block.data(), 4U, 4U, 4U).has_value());
    sl::CwParams event_params = cw_params(4.0, 3.0);
    StageStorage event_storage{};
    const sl::PlacedStage event_stage = sl::construct_cw(event_params, event_storage.bytes.data(), event_storage.bytes.size());
    REQUIRE(event_stage.has_value());
    sl::Impairment& events = event_stage.value().get();
    REQUIRE(events.prepare(sl::StageContext{.reference_rms = 1.0, .total_frames = 20U, .seed = 123U, .index = 7U}).is_ok());
    std::array<float, 20U> event_block{};
    REQUIRE(events.process(event_block.data(), event_block.size(), 0U, event_block.size()).has_value());
    CHECK(events.stats().events_scheduled == 4U);
    CHECK(events.stats().events_applied == 4U);
    params.pulse_period_frames = 8U;
    params.pulse_on_frames = 4U;
    StageStorage conflict_storage{};
    const sl::PlacedStage conflict = sl::construct_cw(params, conflict_storage.bytes.data(), conflict_storage.bytes.size());
    REQUIRE(conflict.has_value());
    CHECK_FALSE(conflict.value().get().prepare(sl::StageContext{.reference_rms = 1.0, .total_frames = 20U}).is_ok());
    StageStorage retry_storage{};
    const sl::PlacedStage retry_stage = sl::construct_cw(cw_params(3.0, 2.0), retry_storage.bytes.data(), retry_storage.bytes.size());
    REQUIRE(retry_stage.has_value());
    sl::Impairment& retry = retry_stage.value().get();
    CHECK_FALSE(retry.prepare(sl::StageContext{.reference_rms = 0.0, .total_frames = 20U}).is_ok());
    CHECK_FALSE(retry.process(block.data(), 1U, 0U, block.size()).has_value());
    CHECK(retry.prepare(sl::StageContext{.reference_rms = 1.0, .total_frames = 20U}).is_ok());
    CHECK(retry.process(block.data(), 1U, 0U, block.size()).has_value());
}
