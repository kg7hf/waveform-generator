// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

#pragma once

// Borrowed, bounded source access for the optional second-source overlay.

#include "common/status.hpp"

#include <cstdint>
#include <span>

namespace signal_lab
{

class SecondarySource
{
public:
    virtual ~SecondarySource() = default;

    [[nodiscard]] virtual std::uint64_t frames() const noexcept = 0;
    [[nodiscard]] virtual double reference_rms() const noexcept = 0;

    // Success means every requested sample was written. Calls are bounded by
    // engine_block_frames; the provider and its data outlive the borrowing stage.
    [[nodiscard]] virtual m110::Status read(std::uint64_t first, std::span<float> output) noexcept = 0;

protected:
    SecondarySource() = default;
    SecondarySource(const SecondarySource&) = default;
    SecondarySource& operator=(const SecondarySource&) = default;
    SecondarySource(SecondarySource&&) = default;
    SecondarySource& operator=(SecondarySource&&) = default;
};

class FixedSecondarySource final : public SecondarySource
{
public:
    FixedSecondarySource(std::span<const float> samples, double reference_rms) noexcept : mSamples{samples}, mReferenceRms{reference_rms} {}

    [[nodiscard]] std::uint64_t frames() const noexcept override { return mSamples.size(); }

    [[nodiscard]] double reference_rms() const noexcept override { return mReferenceRms; }

    [[nodiscard]] m110::Status read(std::uint64_t first, std::span<float> output) noexcept override;

private:
    std::span<const float> mSamples{};
    double mReferenceRms{};
};

} // namespace signal_lab
