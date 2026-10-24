// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

#pragma once

#include "common/status.hpp"

#include <array>
#include <cstdint>

namespace waveform_generator::prepare
{

// Temporary is cleanup eligibility, not permission for automatic deletion.
// Both modes retain the completed WAV and its identity on the card.
enum class Retention : std::uint32_t
{
    temporary = 0U,
    keep = 1U
};

struct Request
{
    std::array<char, 13U> source{};
    std::array<char, 13U> scenario{};
    std::array<char, 13U> output{};
    Retention retention{Retention::keep};
    bool seed_override{};
    std::uint64_t seed{};
};

enum class Phase : std::uint32_t
{
    idle = 0U,
    starting = 1U,
    hashing_source = 2U,
    hashing_secondary = 3U,
    calibrating = 4U,
    rendering = 5U,
    verifying = 6U,
    publishing = 7U,
    complete = 8U,
    cancelled = 9U,
    failed = 10U,
    cleanup_failed = 11U,
};

struct Snapshot
{
    Phase phase{Phase::idle};
    m110::Status status{};
    Request request{};
    std::uint64_t input_frames{};
    std::uint64_t output_frames{};
    std::uint64_t total_input_frames{};
    std::uint64_t seed{};
    std::array<char, 65U> wav_sha256{};
    std::array<char, 65U> pcm_sha256{};
};

} // namespace waveform_generator::prepare
