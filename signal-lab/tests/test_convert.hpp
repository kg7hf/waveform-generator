// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

#pragma once

// The test suites' own conversions, each written once, beside signal-lab's
// (signal_lab/convert.hpp, which the suites use for everything else): the signed
// frame arithmetic of the reference models, which index before frame 0, and the
// distance type of the standard library's iterators, the boundary the suites call.

#include <cstddef>
#include <cstdint>

namespace signal_lab_test
{

// A frame index or count below 2^63 as a signed frame: exact.
[[nodiscard]] constexpr std::int64_t signed_frame(std::uint64_t frame) noexcept
{
    return static_cast<std::int64_t>(frame);
}

// A signed frame the caller has checked to be >= 0, as an index: exact.
[[nodiscard]] constexpr std::size_t index_of(std::int64_t frame) noexcept
{
    return static_cast<std::size_t>(frame);
}

// A non-negative iterator distance (std::ptrdiff_t) as a count: exact.
[[nodiscard]] constexpr std::size_t count_of(std::ptrdiff_t distance) noexcept
{
    return static_cast<std::size_t>(distance);
}

// A count below 2^63 as an iterator offset: exact.
[[nodiscard]] constexpr std::ptrdiff_t offset_of(std::size_t count) noexcept
{
    return static_cast<std::ptrdiff_t>(count);
}

} // namespace signal_lab_test
