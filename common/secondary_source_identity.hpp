// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

#pragma once

#include <cstdint>

namespace waveform_generator
{

// The measured source excerpt used by one accepted play. All zero means no
// configured second source. Copy this value before a later preflight reuses
// provider storage; it contains no borrowed path or file handle. PCM24 RIFF
// file limits bound all three frame counts to uint32_t on the target.
struct SecondarySourceIdentity
{
    std::uint32_t version{};
    std::uint32_t source_frames{};
    std::uint32_t excerpt_start_frame{};
    std::uint32_t excerpt_frames{};
    std::uint32_t digest_hi{};
    std::uint32_t digest_lo{};
    std::uint32_t latency_frames{};
    double reference_rms{};

    constexpr bool operator==(const SecondarySourceIdentity&) const noexcept = default;
};

} // namespace waveform_generator
