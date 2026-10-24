// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// The PCM16 conversions of the HF lane's pathsim chain (guide: docs/modules/pathsim.md).
//
// The chain has two different PCM16 writers, and both are reproduced exactly:
//  - pathsim (AudioFile.h) reads s / 32768 and writes trunc(clamp(x, -1, 1) * 32767);
//  - the lane resampler (m110_wav_resample) reads s / 32768 (PCM24 s / 8388608) and
//    writes lround(clamp(x * 32767, -32768, 32767)), rounding half away from zero.
// These functions are the one place where samples cross between double and int16.

#pragma once

#include <cstdint>

namespace channel_sim
{

using Pcm16 = std::int16_t;

// Full-scale divisor on input (both programs) and multiplier on output (both programs).
inline constexpr double pcm16_input_scale = 32768.0;
inline constexpr double pcm16_output_scale = 32767.0;
// The PCM24 divisor of the lane resampler's reader.
inline constexpr double pcm24_input_scale = 8388608.0;

// pathsim's reader (AudioFile::sixteenBitIntToSample): exact.
[[nodiscard]] constexpr double pcm16_to_unit(Pcm16 sample) noexcept
{
    return static_cast<double>(sample) / pcm16_input_scale;
}

// The lane resampler's PCM24 reader: exact (a 24-bit integer over 2^23).
[[nodiscard]] constexpr double pcm24_to_unit(std::int32_t sample) noexcept
{
    return static_cast<double>(sample) / pcm24_input_scale;
}

// pathsim's writer (AudioFile::sampleToSixteenBitInt): clamp to [-1, 1] (min, then
// max, as AudioFile::clamp), scale by 32767 and truncate toward zero. NaN gives 0,
// which is what the x86 conversion produces for pathsim; here it is defined.
[[nodiscard]] Pcm16 unit_to_pcm16_truncated(double value) noexcept;

// The lane resampler's writer (write_pcm16_wav): std::clamp(value * 32767, -32768,
// 32767), then round half away from zero (std::lround). NaN gives 0.
[[nodiscard]] Pcm16 unit_to_pcm16_rounded(double value) noexcept;

} // namespace channel_sim
