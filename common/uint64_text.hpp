// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// Bounded integer text for embedded manifests and status replies.
// Guide: docs/modules/protocols-media-and-artifacts.md
// Ownership: returns its own NUL-terminated storage; no allocation or I/O.
// Contract: decimal or lowercase hexadecimal, optionally zero-padded.
// newlib-nano printf lacks long-long conversions: pass this text through %s.
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace waveform_generator
{
namespace detail
{

// uint64_t needs 20 decimal digits at most; the spare byte is the NUL terminator.
inline constexpr std::size_t uint64_decimal_max_digits = 20U;
inline constexpr std::size_t uint64_hexadecimal_max_digits = 16U;
inline constexpr std::size_t uint64_text_storage_size = uint64_decimal_max_digits + 1U;

} // namespace detail

template <unsigned Base = 10U, std::size_t MinimumDigits = 1U> [[nodiscard]] constexpr std::array<char, detail::uint64_text_storage_size> uint64_text(std::uint64_t value) noexcept
{
    // 50K view: provide bounded decimal or lowercase hexadecimal text for embedded output.
    // Detailed view: repeated division emits least-significant digits first, then reverses them.
    // 5th-grade view: write the digits backwards, then turn them around.
    static_assert(Base == 10U || Base == 16U);
    constexpr std::size_t maximum_digits = Base == 10U ? detail::uint64_decimal_max_digits : detail::uint64_hexadecimal_max_digits;
    static_assert(MinimumDigits >= 1U && MinimumDigits <= maximum_digits);
    constexpr std::string_view digits{"0123456789abcdef"};
    std::array<char, detail::uint64_text_storage_size> result{};
    std::size_t count{};

    do
    {
        result[count++] = digits[static_cast<std::size_t>(value % Base)];
        value /= Base;
    } while (value != 0U || count < MinimumDigits);

    std::reverse(result.begin(), result.begin() + static_cast<std::ptrdiff_t>(count));
    return result;
}

} // namespace waveform_generator
