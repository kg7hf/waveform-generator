// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

#include "common/operation_timing.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <limits>

namespace timing = waveform_generator::operation_timing;

namespace
{

constexpr std::uint32_t cycle_hz = 100000000U;
constexpr std::uint32_t tick_hz = 1000U;

constexpr timing::Sample sample(std::uint32_t cycles, std::uint32_t ticks, std::uint32_t frequency = cycle_hz) noexcept
{
    return {.cycles = cycles, .ticks = ticks, .frequency_hz = frequency};
}

} // namespace

TEST_CASE("Operation timing accumulates elapsed cycles across both counter wraps", "[operation-timing]")
{
    timing::Accumulator accumulator;
    accumulator.reset(cycle_hz, tick_hz);
    accumulator.record(timing::Phase::render, sample(0xFFFFFFF0U, 10U), sample(0x00000020U, 11U));
    accumulator.record(timing::Phase::render, sample(100U, 20U), sample(250U, 22U));
    const auto snapshot = accumulator.snapshot();
    const auto& render = snapshot.phases[static_cast<std::size_t>(timing::Phase::render)];
    CHECK(snapshot.cycle_hz == cycle_hz);
    CHECK(snapshot.tick_hz == tick_hz);
    CHECK(render.calls == 2U);
    CHECK(render.total_cycles == 48U + 150U);
    CHECK(render.max_cycles == 150U);
    CHECK(render.rejected == 0U);
    CHECK_FALSE(render.saturated);
}

TEST_CASE("Operation timing rejects ambiguous long spans and invalid clocks", "[operation-timing]")
{
    timing::Accumulator accumulator;
    accumulator.reset(cycle_hz, tick_hz);
    // At 100 MHz / 1 kHz the cycle counter wraps in about 42.95 seconds.
    // 43 seconds plus the sampling allowance is ambiguous and must be rejected.
    accumulator.record(timing::Phase::append, sample(100U, 7U), sample(200U, 43007U));
    accumulator.record(timing::Phase::append, sample(100U, 50U), sample(200U, 51U, cycle_hz + 1U));
    accumulator.record(timing::Phase::append, sample(100U, 50U, cycle_hz + 1U), sample(200U, 51U));
    auto snapshot = accumulator.snapshot();
    auto append = snapshot.phases[static_cast<std::size_t>(timing::Phase::append)];
    CHECK(append.calls == 3U);
    CHECK(append.rejected == 3U);
    CHECK(append.total_cycles == 0U);
    CHECK(append.max_cycles == 0U);
    accumulator.reset(0U, tick_hz);
    accumulator.record(timing::Phase::cleanup, sample(1U, 1U, 0U), sample(2U, 2U, 0U));
    snapshot = accumulator.snapshot();
    const auto& cleanup = snapshot.phases[static_cast<std::size_t>(timing::Phase::cleanup)];
    CHECK(cleanup.calls == 1U);
    CHECK(cleanup.rejected == 1U);
    accumulator.reset(cycle_hz, 0U);
    accumulator.record(timing::Phase::cleanup, sample(1U, 1U), sample(2U, 2U));
    snapshot = accumulator.snapshot();
    CHECK(snapshot.phases[static_cast<std::size_t>(timing::Phase::cleanup)].rejected == 1U);
    accumulator.reset(std::numeric_limits<std::uint32_t>::max(), std::numeric_limits<std::uint32_t>::max());
    accumulator.record(timing::Phase::cleanup, {.cycles = 0U, .ticks = 0U, .frequency_hz = std::numeric_limits<std::uint32_t>::max()},
                       {.cycles = 1U, .ticks = std::numeric_limits<std::uint32_t>::max(), .frequency_hz = std::numeric_limits<std::uint32_t>::max()});
    snapshot = accumulator.snapshot();
    const auto& edge = snapshot.phases[static_cast<std::size_t>(timing::Phase::cleanup)];
    CHECK(edge.calls == 1U);
    CHECK(edge.rejected == 1U);
}

TEST_CASE("Operation timing validates exact conservative wrap threshold", "[operation-timing]")
{
    timing::Accumulator accumulator;
    accumulator.reset(1000000U, 1000U);
    // With 1 MHz / 1 kHz, the two-tick allowance makes 4,294,965 observed
    // ticks just below the 2^32 cycle threshold, while 4,294,966 exceeds it.
    accumulator.record(timing::Phase::commit, {.cycles = 0U, .ticks = 0U, .frequency_hz = 1000000U}, {.cycles = 1U, .ticks = 4294965U, .frequency_hz = 1000000U});
    accumulator.record(timing::Phase::commit, {.cycles = 2U, .ticks = 0U, .frequency_hz = 1000000U}, {.cycles = 3U, .ticks = 4294966U, .frequency_hz = 1000000U});
    const auto snapshot = accumulator.snapshot();
    const auto& commit = snapshot.phases[static_cast<std::size_t>(timing::Phase::commit)];
    CHECK(commit.calls == 2U);
    CHECK(commit.rejected == 1U);
    CHECK(commit.total_cycles == 1U);
}

TEST_CASE("Operation timing accepts an RTOS tick counter wrap", "[operation-timing]")
{
    timing::Accumulator accumulator;
    accumulator.reset(cycle_hz, tick_hz);
    accumulator.record(timing::Phase::source_read, sample(25U, std::numeric_limits<std::uint32_t>::max() - 1U), sample(75U, 1U));
    const auto snapshot = accumulator.snapshot();
    const auto& read = snapshot.phases[static_cast<std::size_t>(timing::Phase::source_read)];
    CHECK(read.calls == 1U);
    CHECK(read.rejected == 0U);
    CHECK(read.total_cycles == 50U);
}

TEST_CASE("Operation timing reset clears every phase and records zero-delta calls", "[operation-timing]")
{
    timing::Accumulator accumulator;
    accumulator.reset(cycle_hz, tick_hz);
    accumulator.record(timing::Phase::read, sample(7U, 9U), sample(7U, 9U));
    const auto before_reset = accumulator.snapshot();
    const auto& zero_delta = before_reset.phases[static_cast<std::size_t>(timing::Phase::read)];
    CHECK(zero_delta.calls == 1U);
    CHECK(zero_delta.total_cycles == 0U);
    accumulator.reset(200000000U, 1000U);
    const auto snapshot = accumulator.snapshot();
    CHECK(snapshot.cycle_hz == 200000000U);
    CHECK(snapshot.tick_hz == tick_hz);

    for (const auto& stats : snapshot.phases)
    {
        CHECK(stats.calls == 0U);
        CHECK(stats.total_cycles == 0U);
        CHECK(stats.max_cycles == 0U);
        CHECK(stats.rejected == 0U);
        CHECK_FALSE(stats.saturated);
    }

    accumulator.record(timing::Phase::read, {.cycles = 1U, .ticks = 2U, .frequency_hz = 200000000U}, {.cycles = 1U, .ticks = 2U, .frequency_hz = 200000000U});
    CHECK(accumulator.snapshot().phases[static_cast<std::size_t>(timing::Phase::read)].calls == 1U);
}

TEST_CASE("Operation timing aggregate arithmetic saturates instead of wrapping", "[operation-timing]")
{
    bool saturated = false;
    const auto sum = timing::detail::saturating_add(std::numeric_limits<std::uint64_t>::max() - 3U, 5U, saturated);
    CHECK(sum == std::numeric_limits<std::uint64_t>::max());
    CHECK(saturated);
    saturated = false;
    const auto exact = timing::detail::saturating_add(std::numeric_limits<std::uint64_t>::max() - 3U, 3U, saturated);
    CHECK(exact == std::numeric_limits<std::uint64_t>::max());
    CHECK_FALSE(saturated);
}

TEST_CASE("Operation timing marks an invalid phase as rejected", "[operation-timing]")
{
    timing::Accumulator accumulator;
    accumulator.reset(cycle_hz, tick_hz);
    accumulator.record(static_cast<timing::Phase>(255U), sample(1U, 1U), sample(20U, 2U));
    const auto snapshot = accumulator.snapshot();
    const auto& unclassified = snapshot.phases[static_cast<std::size_t>(timing::Phase::setup)];
    CHECK(unclassified.calls == 1U);
    CHECK(unclassified.rejected == 1U);
    CHECK(unclassified.total_cycles == 0U);
}
