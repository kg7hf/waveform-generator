// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

#pragma once

// Sampling quantities: the integral rate, counts and positions of a sample stream.
// Guide: docs/modules/signal-lab-watterson.md, "Types and units".
//
// The design is modem-core's (modem/common/units/sampling.hpp), copied, not
// shared: signal-lab must not depend on modem-core. A rate, a count and an
// absolute position are distinct types, so a count cannot be passed where a
// position belongs. The algebra is the affine one: index - index is a count,
// index +/- count is an index, and index + index does not compile.

#include <compare>
#include <cstdint>

namespace signal_lab::units
{

// Samples per second.
class SampleRate
{
public:
    constexpr SampleRate() noexcept = default;

    constexpr explicit SampleRate(std::uint32_t hertz) noexcept : mHertz{hertz} {}

    [[nodiscard]] constexpr std::uint32_t hertz() const noexcept { return mHertz; }

    [[nodiscard]] constexpr auto operator<=>(const SampleRate&) const noexcept = default;

private:
    std::uint32_t mHertz{};
};

// A number of samples (frames of a mono stream): a length, a delay, a period.
class SampleCount
{
public:
    constexpr SampleCount() noexcept = default;

    constexpr explicit SampleCount(std::uint64_t samples) noexcept : mSamples{samples} {}

    [[nodiscard]] constexpr std::uint64_t value() const noexcept { return mSamples; }

    [[nodiscard]] constexpr auto operator<=>(const SampleCount&) const noexcept = default;

    [[nodiscard]] constexpr SampleCount operator+(SampleCount other) const noexcept { return SampleCount{mSamples + other.mSamples}; }

    [[nodiscard]] constexpr SampleCount operator-(SampleCount other) const noexcept { return SampleCount{mSamples - other.mSamples}; }

    [[nodiscard]] constexpr SampleCount operator*(std::uint64_t factor) const noexcept { return SampleCount{mSamples * factor}; }

    [[nodiscard]] constexpr SampleCount operator/(std::uint64_t divisor) const noexcept { return SampleCount{mSamples / divisor}; }

private:
    std::uint64_t mSamples{};
};

// An absolute position in the stream: the 64-bit frame counter (survey gap A8).
class SampleIndex
{
public:
    constexpr SampleIndex() noexcept = default;

    constexpr explicit SampleIndex(std::uint64_t index) noexcept : mIndex{index} {}

    [[nodiscard]] constexpr std::uint64_t value() const noexcept { return mIndex; }

    [[nodiscard]] constexpr auto operator<=>(const SampleIndex&) const noexcept = default;

    // Unsigned arithmetic: a position before the stream start wraps modulo 2^64,
    // which a power-of-two ring indexes correctly (the ring reads zeros there).
    [[nodiscard]] constexpr SampleIndex operator+(SampleCount count) const noexcept { return SampleIndex{mIndex + count.value()}; }

    [[nodiscard]] constexpr SampleIndex operator-(SampleCount count) const noexcept { return SampleIndex{mIndex - count.value()}; }

    [[nodiscard]] constexpr SampleCount operator-(SampleIndex earlier) const noexcept { return SampleCount{mIndex - earlier.mIndex}; }

    constexpr SampleIndex& operator+=(SampleCount count) noexcept
    {
        mIndex += count.value();
        return *this;
    }

private:
    std::uint64_t mIndex{};
};

} // namespace signal_lab::units
