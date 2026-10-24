// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

#pragma once

#include <cstdint>

namespace signal_lab
{

struct StageRandomKey
{
    std::uint64_t seed{};
    std::uint32_t index{};
};

inline constexpr std::uint32_t stream_identity_algorithm_version = 1U;
inline constexpr std::uint64_t stream_identity_domain = 0x5746475354524D31ULL; // "WFGSTRM1"

// Explicit streams are stable under stage insertion/reordering. Stream zero keeps
// the legacy ordinal-derived key. This is SplitMix64's next-word transform.
[[nodiscard]] constexpr StageRandomKey resolve_stage_random_key(std::uint64_t master_seed, std::uint32_t stage_index, std::uint32_t stream_id) noexcept
{
    if (stream_id == 0U)
    {
        return StageRandomKey{master_seed, stage_index};
    }

    std::uint64_t value = master_seed ^ stream_identity_domain ^ static_cast<std::uint64_t>(stream_id);
    value += 0x9E3779B97F4A7C15ULL;
    value = (value ^ (value >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    value = (value ^ (value >> 27U)) * 0x94D049BB133111EBULL;
    value ^= value >> 31U;
    return StageRandomKey{value, 0U};
}

} // namespace signal_lab
