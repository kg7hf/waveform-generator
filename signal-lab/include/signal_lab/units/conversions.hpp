// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

#pragma once

// The conversions between the unit types, one function each.
// Guide: docs/modules/signal-lab-watterson.md, "Types and units".

#include "common/status.hpp"
#include "signal_lab/units/duration.hpp"
#include "signal_lab/units/frequency.hpp"
#include "signal_lab/units/sampling.hpp"

#include <compare>
#include <cstdint>

namespace signal_lab::units
{

// 2^32: one turn of a 32-bit phase accumulator.
inline constexpr double phase_turn = 4294967296.0;

// A per-frame phase increment of a 32-bit accumulator: round(f 2^32 / fs) in
// two's complement, so a negative shift is a word above 2^31. The step is
// fs / 2^32 (11.2 uHz at 48 kHz), so the realised shift is within 5.6 uHz.
class PhaseStep
{
public:
    constexpr PhaseStep() noexcept = default;

    constexpr explicit PhaseStep(std::uint32_t word) noexcept : mWord{word} {}

    [[nodiscard]] constexpr std::uint32_t word() const noexcept { return mWord; }

    [[nodiscard]] constexpr auto operator<=>(const PhaseStep&) const noexcept = default;

private:
    std::uint32_t mWord{};
};

// Whole numbers of frames below 2^53 convert between double and integer exactly; a
// conversion refuses anything beyond (and the integer casts never see it).
inline constexpr double exact_frame_limit = 9007199254740992.0;

// E.5.2: a delay as a whole number of frames, rounded to the nearest frame with
// ties to even. Formed as (ms x fs) / 1000, so a whole or binary-fraction number
// of milliseconds converts with a single rounding. Refused (invalid_argument, a
// static message) unless the delay is finite and >= 0, the rate is not zero and the
// count is below 2^53: the same rule on host and board, never a wrapped cast.
[[nodiscard]] m110::Result<SampleCount> to_frames(Duration delay, SampleRate rate) noexcept;

[[nodiscard]] Duration to_duration(SampleCount frames, SampleRate rate) noexcept;

// round(f 2^32 / fs), ties to even. Refused unless the shift is finite with
// |f| <= fs / 2 and the rate is not zero.
[[nodiscard]] m110::Result<PhaseStep> phase_step(FrequencyDelta shift, SampleRate rate) noexcept;

// The shift a phase step realises: the signed word times fs / 2^32.
[[nodiscard]] FrequencyDelta realised_shift(PhaseStep step, SampleRate rate) noexcept;

} // namespace signal_lab::units
