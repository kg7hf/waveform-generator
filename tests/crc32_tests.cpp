// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// CRC-32 against zlib's published values, an independent bitwise reference,
// and incremental-versus-one-shot equivalence over every split point.

#include "common/crc32.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace
{
namespace wg = waveform_generator;

std::vector<std::uint8_t> bytes_of(std::string_view text)
{
    return {text.begin(), text.end()};
}

// Bit-at-a-time CRC-32 with no table: an independent check of the table path.
std::uint32_t bitwise_crc32(std::span<const std::uint8_t> bytes) noexcept
{
    std::uint32_t remainder = wg::crc32_initial;

    for (const std::uint8_t byte : bytes)
    {
        remainder ^= byte;

        for (std::uint32_t bit = 0U; bit < wg::crc32_bits_per_byte; ++bit)
        {
            remainder = (remainder & 1U) != 0U ? (remainder >> 1U) ^ wg::crc32_polynomial : remainder >> 1U;
        }
    }

    return remainder ^ wg::crc32_final_xor;
}

// Deterministic bytes: the top byte of a 32-bit LCG (Numerical Recipes constants).
std::vector<std::uint8_t> pattern(std::size_t size)
{
    constexpr std::uint32_t multiplier = 1664525U;
    constexpr std::uint32_t increment = 1013904223U;
    constexpr std::size_t top_byte = sizeof(std::uint32_t) - 1U;
    static_assert(std::endian::native == std::endian::little);
    std::vector<std::uint8_t> bytes(size);
    std::uint32_t state = 1U;

    for (auto& byte : bytes)
    {
        state = state * multiplier + increment;
        byte = std::get<top_byte>(std::bit_cast<std::array<std::uint8_t, sizeof(state)>>(state));
    }

    return bytes;
}

TEST_CASE("CRC-32 known values match zlib", "[crc32]")
{
    // Entries 1 and 255 of zlib's crc_table, and zlib.crc32() of each input.
    constexpr std::uint32_t table_entry_1 = 0x77073096U;
    constexpr std::uint32_t table_entry_255 = 0x2D02EF8DU;
    constexpr std::uint32_t fox_crc = 0x414FA339U;
    constexpr std::uint32_t a_crc = 0xE8B7BE43U;
    static_assert(std::get<1>(wg::crc32_table) == table_entry_1 && std::get<wg::crc32_table_size - 1U>(wg::crc32_table) == table_entry_255, "the generated table is zlib's");
    CHECK(wg::crc32(wg::crc32_check_input) == wg::crc32_check_value);
    CHECK(wg::crc32(bytes_of("123456789")) == wg::crc32_check_value);
    CHECK(wg::crc32(bytes_of("The quick brown fox jumps over the lazy dog")) == fox_crc);
    CHECK(wg::crc32(bytes_of("a")) == a_crc);
    CHECK(wg::crc32({}) == 0U);
}

TEST_CASE("CRC-32 incremental updates match one-shot updates", "[crc32]")
{
    constexpr std::size_t size = 1027U;
    const auto bytes = pattern(size);
    const std::span<const std::uint8_t> all{bytes};
    const auto expected = wg::crc32(all);
    CHECK(expected == bitwise_crc32(all));
    bool every_split = true;

    for (std::size_t split = 0U; split <= all.size(); ++split)
    {
        wg::Crc32 crc;
        crc.update(all.first(split));
        crc.update(all.subspan(split));
        every_split = every_split && crc.value() == expected;
    }

    CHECK(every_split);
    // Three-byte pieces, as a PCM24 sample stream feeds it.
    constexpr std::size_t piece = 3U;
    wg::Crc32 pieces;

    for (std::size_t at = 0U; at < all.size(); at += piece)
    {
        pieces.update(all.subspan(at, std::min(piece, all.size() - at)));
    }

    CHECK(pieces.value() == expected);
    pieces.reset();
    CHECK(pieces.value() == 0U);
    pieces.update(wg::crc32_check_input);
    CHECK(pieces.value() == wg::crc32_check_value);
}
} // namespace
