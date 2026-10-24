// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace waveform_generator::operation_timing
{

// Sample the cycle counter and RTOS tick close together at each endpoint.
// Frequency is captured with the sample so a clock change cannot silently
// mix cycles from different rates in one operation.
struct Sample
{
    std::uint32_t cycles{};
    std::uint32_t ticks{};
    std::uint32_t frequency_hz{};
};

struct Stats
{
    std::uint64_t calls{};
    std::uint64_t total_cycles{};
    std::uint32_t max_cycles{};
    std::uint64_t rejected{};
    bool saturated{};
};

enum class Phase : std::uint8_t
{
    setup,
    source_read,
    render,
    append,
    commit,
    read,
    finalize,
    cleanup
};

inline constexpr std::size_t phase_count = 8U;

struct Snapshot
{
    std::uint32_t cycle_hz{};
    std::uint32_t tick_hz{};
    std::array<Stats, phase_count> phases{};
};

namespace detail
{

// Exposed in detail so saturation behavior can be tested without billions of
// operation samples. Production aggregation uses this same primitive.
[[nodiscard]] constexpr std::uint64_t saturating_add(std::uint64_t current, std::uint64_t increment, bool& saturated) noexcept
{
    if (increment > std::numeric_limits<std::uint64_t>::max() - current)
    {
        saturated = true;
        return std::numeric_limits<std::uint64_t>::max();
    }

    return current + increment;
}

} // namespace detail

// Fixed-size, OS-independent timing accumulator. Deltas are elapsed core
// cycles, including time blocked in I/O and time spent servicing interrupts;
// they are not a measure of CPU-only execution time. To be counted, an
// individual sample must be shorter than one 32-bit tick-counter wrap, which
// the caller guarantees, and provably shorter than one cycle-counter wrap.
// Longer cycle intervals are rejected as ambiguous.
class Accumulator final
{
public:
    constexpr Accumulator() noexcept = default;

    constexpr void reset(std::uint32_t cycle_hz, std::uint32_t tick_hz) noexcept { snapshot_ = {.cycle_hz = cycle_hz, .tick_hz = tick_hz}; }

    // Every call is counted, including rejected samples. A bad Phase value is
    // attributed to the setup bucket as a rejected sample because the fixed
    // snapshot has no out-of-range bucket; ordinary callers pass Phase values.
    void record(Phase phase, Sample begin, Sample end) noexcept
    {
        const bool phase_valid = valid_phase(phase);
        const std::size_t index = phase_index(phase);
        Stats& stats = snapshot_.phases[index];
        stats.calls = detail::saturating_add(stats.calls, 1U, stats.saturated);

        if (!phase_valid)
        {
            stats.rejected = detail::saturating_add(stats.rejected, 1U, stats.saturated);
            return;
        }

        const std::uint32_t elapsed_cycles = end.cycles - begin.cycles;
        const std::uint32_t elapsed_ticks = end.ticks - begin.ticks;

        if (!valid_samples(begin, end, elapsed_ticks))
        {
            stats.rejected = detail::saturating_add(stats.rejected, 1U, stats.saturated);
            return;
        }

        stats.total_cycles = detail::saturating_add(stats.total_cycles, elapsed_cycles, stats.saturated);

        if (elapsed_cycles > stats.max_cycles)
        {
            stats.max_cycles = elapsed_cycles;
        }
    }

    [[nodiscard]] constexpr Snapshot snapshot() const noexcept { return snapshot_; }

private:
    [[nodiscard]] static constexpr bool valid_phase(Phase phase) noexcept { return static_cast<std::size_t>(phase) < phase_count; }

    [[nodiscard]] static constexpr std::size_t phase_index(Phase phase) noexcept
    {
        const auto index = static_cast<std::size_t>(phase);
        return index < phase_count ? index : 0U;
    }

    [[nodiscard]] bool valid_samples(Sample begin, Sample end, std::uint32_t elapsed_ticks) const noexcept
    {
        if (snapshot_.cycle_hz == 0U || snapshot_.tick_hz == 0U || begin.frequency_hz != snapshot_.cycle_hz || end.frequency_hz != snapshot_.cycle_hz)
        {
            return false;
        }

        // Two tick periods cover endpoint sampling skew. A span that could
        // reach a complete CYCCNT wrap cannot be disambiguated from its modulo
        // delta, even if the observed cycle delta looks small.
        const std::uint64_t conservative_ticks = static_cast<std::uint64_t>(elapsed_ticks) + 2U;
        constexpr std::uint64_t cycle_wrap = std::uint64_t{1U} << 32U;
        const std::uint64_t wrap_limit = cycle_wrap * snapshot_.tick_hz;
        const std::uint64_t largest_safe_tick_count = (wrap_limit - 1U) / snapshot_.cycle_hz;
        return conservative_ticks <= largest_safe_tick_count;
    }

    Snapshot snapshot_{};
};

} // namespace waveform_generator::operation_timing
