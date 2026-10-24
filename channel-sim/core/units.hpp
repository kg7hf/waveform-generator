// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// Named units for the channel-sim chain (guide: docs/modules/pathsim.md).
// Each quantity is a one-field aggregate, so a call site spells its unit:
// Decibels{17.6}, Hertz{1.0}, Milliseconds{2.0}.

#pragma once

#include <cstdint>

namespace channel_sim
{

// Samples per second of one channel.
struct SampleRate
{
    std::uint32_t hz{};

    friend constexpr bool operator==(SampleRate, SampleRate) noexcept = default;
};

// A level ratio in dB (20 log10 of an amplitude ratio).
struct Decibels
{
    double value{};
};

// A frequency in Hz (Doppler spread is the two-sigma width, as pathsim labels it).
struct Hertz
{
    double value{};
};

// A time in milliseconds (path delays).
struct Milliseconds
{
    double value{};
};

// A time in seconds.
struct Seconds
{
    double value{};
};

// The pathsim realization seed (pathsim --seed). Without --seed pathsim uses 1.
struct Seed
{
    std::uint32_t value{1U};
};

// pathsim works at a fixed 8 kHz; the lane's audio is 48 kHz.
inline constexpr SampleRate pathsim_sample_rate{8000U};
inline constexpr SampleRate lane_sample_rate{48000U};

} // namespace channel_sim
