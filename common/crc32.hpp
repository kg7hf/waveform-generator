// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// zlib-compatible CRC-32 (IEEE 802.3): reflected, polynomial 0xEDB88320,
// initial value and final XOR 0xFFFFFFFF
// Guide: docs/modules/protocols-media-and-artifacts.md
// Ownership: a Crc32 is a plain value; each instance has one owner.
// Contract: update() over any split of the input gives the same value() as one
// call over the whole input, equal to Python's zlib.crc32(). No allocation, no
// branches on data, constant time per byte: safe in an ISR.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace waveform_generator
{

inline constexpr std::uint32_t crc32_polynomial = 0xEDB88320U;
inline constexpr std::uint32_t crc32_initial = 0xFFFFFFFFU;
inline constexpr std::uint32_t crc32_final_xor = 0xFFFFFFFFU;
inline constexpr std::size_t crc32_table_size = 256U;
inline constexpr std::uint32_t crc32_bits_per_byte = 8U;
inline constexpr std::uint32_t crc32_byte_mask = 0xFFU;

[[nodiscard]] constexpr std::array<std::uint32_t, crc32_table_size> make_crc32_table() noexcept
{
    std::array<std::uint32_t, crc32_table_size> table{};
    std::uint32_t byte = 0U;

    for (auto& entry : table)
    {
        std::uint32_t remainder = byte;

        for (std::uint32_t bit = 0U; bit < crc32_bits_per_byte; ++bit)
        {
            remainder = (remainder & 1U) != 0U ? (remainder >> 1U) ^ crc32_polynomial : remainder >> 1U;
        }

        entry = remainder;
        ++byte;
    }

    return table;
}

inline constexpr std::array<std::uint32_t, crc32_table_size> crc32_table = make_crc32_table();

class Crc32
{
public:
    // Continue the checksum over `bytes` (borrowed for the call only).
    constexpr void update(std::span<const std::uint8_t> bytes) noexcept
    {
        // The index is masked to one byte and the table has an entry for every
        // byte value (static_assert below), so the lookup is always in bounds.
        const std::span<const std::uint32_t, crc32_table_size> table{crc32_table};

        for (const std::uint8_t byte : bytes)
        {
            const std::uint32_t index = (state_ ^ byte) & crc32_byte_mask;
            state_ = (state_ >> crc32_bits_per_byte) ^ table[index];
        }
    }

    // The CRC of everything given to update() since construction or reset().
    [[nodiscard]] constexpr std::uint32_t value() const noexcept { return state_ ^ crc32_final_xor; }

    constexpr void reset() noexcept { state_ = crc32_initial; }

private:
    std::uint32_t state_{crc32_initial};
};

[[nodiscard]] constexpr std::uint32_t crc32(std::span<const std::uint8_t> bytes) noexcept
{
    Crc32 crc;
    crc.update(bytes);
    return crc.value();
}

// The standard check value: CRC-32 of the ASCII digits "123456789".
inline constexpr std::array<std::uint8_t, 9> crc32_check_input{'1', '2', '3', '4', '5', '6', '7', '8', '9'};
inline constexpr std::uint32_t crc32_check_value = 0xCBF43926U;

static_assert(crc32_table.size() == std::size_t{crc32_byte_mask} + 1U, "every byte value indexes the table");
static_assert(crc32(crc32_check_input) == crc32_check_value, "CRC-32 check value");
static_assert(crc32({}) == 0U, "the CRC of no bytes is zero");

} // namespace waveform_generator
