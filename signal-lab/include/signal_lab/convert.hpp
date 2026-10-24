// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

#pragma once

// signal-lab's numeric conversions, each written once (C++ Core Guidelines
// ES.46, ES.48, ES.49; the pattern of channel-sim/core/convert.hpp).
// Guide: docs/modules/signal-lab-watterson.md, "Types and units".
//
// The Watterson stage, the noise stage and the unit types mix sample counts
// (std::uint64_t), 32-bit phases, doubles for configuration and floats for the
// sample path. These helpers are the only casts between them in the new code,
// so a reader finds every narrowing in one place. Each states when it is exact.

#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>

namespace signal_lab
{

// An integer count, index or draw as a double: exact below 2^53, which every
// frame count, node index and 32-bit draw in signal-lab is.
template <std::integral Integer> [[nodiscard]] constexpr double as_double(Integer value) noexcept
{
    return static_cast<double>(value);
}

// A float sample as a double: always exact (binary32 widens into binary64).
[[nodiscard]] constexpr double as_double(float value) noexcept
{
    return static_cast<double>(value);
}

// A double as the nearest float (IEEE round to nearest, ties to even). The
// sample path is float; configuration values are designed in double and
// stored once with this conversion.
[[nodiscard]] constexpr float to_float(double value) noexcept
{
    return static_cast<float>(value);
}

// The low 32 bits of a 64-bit value: arithmetic modulo 2^32, as a phase
// accumulator needs.
[[nodiscard]] constexpr std::uint32_t low_word(std::uint64_t value) noexcept
{
    return static_cast<std::uint32_t>(value);
}

// A signed phase step as its two's-complement word (modulo 2^32).
[[nodiscard]] constexpr std::uint32_t twos_complement(std::int64_t value) noexcept
{
    return static_cast<std::uint32_t>(value);
}

// A 32-bit word as a signed phase step, the inverse of twos_complement().
[[nodiscard]] constexpr std::int32_t as_signed(std::uint32_t word) noexcept
{
    return static_cast<std::int32_t>(word);
}

// A whole-numbered double in [-2^63, 2^63) as an integer. The callers round
// first (det::round_half_even) and bound the value, so the result is exact.
[[nodiscard]] constexpr std::int64_t whole_to_int64(double whole) noexcept
{
    return static_cast<std::int64_t>(whole);
}

// A whole-numbered, non-negative double below 2^64 as an unsigned integer;
// the callers bound it first, so the result is exact.
[[nodiscard]] constexpr std::uint64_t whole_to_uint64(double whole) noexcept
{
    return static_cast<std::uint64_t>(whole);
}

// The smallest integer >= value, for 0 <= value < 2^32: a constexpr ceil for
// filter lengths that size arrays at compile time.
[[nodiscard]] constexpr std::uint32_t ceil_to_uint32(double value) noexcept
{
    const auto truncated = static_cast<std::uint32_t>(value);
    return as_double(truncated) < value ? truncated + 1U : truncated;
}

// The largest integer <= value, for 0 <= value < 2^32 (frames per node).
[[nodiscard]] constexpr std::uint32_t floor_to_uint32(double value) noexcept
{
    return static_cast<std::uint32_t>(value);
}

// A count that the caller has bounded by an array capacity, as an index.
// Exact: every such count is below 2^32, which std::size_t holds on every target.
[[nodiscard]] constexpr std::size_t to_size(std::uint64_t count) noexcept
{
    return static_cast<std::size_t>(count);
}

// A size that the caller has bounded by a compile-time capacity, as a 32-bit count.
[[nodiscard]] constexpr std::uint32_t narrow_count(std::size_t count) noexcept
{
    return static_cast<std::uint32_t>(count);
}

// The IEEE-754 bits of a double, for digests: the value itself, not its text.
[[nodiscard]] constexpr std::uint64_t bits_of(double value) noexcept
{
    return std::bit_cast<std::uint64_t>(value);
}

} // namespace signal_lab
