// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Paul R. Decker

#pragma once

#include <cstdint>

namespace waveform_generator::audio
{

using Pcm24Sample = std::int32_t;

inline constexpr Pcm24Sample pcm24_min = -8388608;
inline constexpr Pcm24Sample pcm24_max = 8388607;
inline constexpr double pcm24_scale = 8388608.0;
inline constexpr std::uint32_t pcm24_bytes_per_sample = 3U;

[[nodiscard]] constexpr float pcm24_to_float(Pcm24Sample sample) noexcept
{
    return static_cast<float>(static_cast<double>(sample) / pcm24_scale);
}

[[nodiscard]] constexpr Pcm24Sample read_pcm24_le(const std::uint8_t* bytes) noexcept
{
    const std::uint32_t value = static_cast<std::uint32_t>(bytes[0]) |
                                (static_cast<std::uint32_t>(bytes[1]) << 8U) |
                                (static_cast<std::uint32_t>(bytes[2]) << 16U);
    return value >= 0x00800000U
           ? static_cast<Pcm24Sample>(value) - 0x01000000
           : static_cast<Pcm24Sample>(value);
}

constexpr void write_pcm24_le(std::uint8_t* bytes, Pcm24Sample sample) noexcept
{
    const auto value = static_cast<std::uint32_t>(sample) & 0x00FFFFFFU;
    bytes[0] = static_cast<std::uint8_t>(value);
    bytes[1] = static_cast<std::uint8_t>(value >> 8U);
    bytes[2] = static_cast<std::uint8_t>(value >> 16U);
}

} // namespace waveform_generator::audio
