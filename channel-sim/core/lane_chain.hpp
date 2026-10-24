// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// The HF lane's 48 kHz pathsim chain, streamed (guide: docs/modules/pathsim.md).
//
// run_pathsim.py (HF lane packet 3a; the campaign's seeded path) runs:
//   1. m110_wav_resample in.wav clean-8k.wav 8000 -12    48 kHz -> 8 kHz, -12 dB, PCM16 rounded
//   2. the campaign's PCM16 rewrite                      no sample changes
//   3. pathsim.exe <options> trial-in.wav trial-chan.wav
//   4. the campaign's PCM16 rewrite                      no sample changes
//   5. m110_wav_resample trial-out-8k.wav out.wav 48000  8 kHz -> 48 kHz, 0 dB, PCM16 rounded
// LaneChain composes LaneDownsampler -> PathsimDriver -> LaneUpsampler with the same
// arithmetic. pathsim's level pre-pass rewinds the 48 kHz source, so the source is
// read and downsampled twice; nothing is stored between the stages.
//
// Memory: about 130 KB in place (two resamplers, the driver, pathsim's processor),
// plus pathsim's own heap use (see pathsim_driver.hpp).

#pragma once

#include "common/status.hpp"
#include "core/lane_resampler.hpp"
#include "core/pathsim_driver.hpp"
#include "core/sample_stream.hpp"
#include "core/units.hpp"

#include <cstdint>

namespace channel_sim
{

// The campaign's 48 -> 8 kHz resampler gain (pathsim-campaign.py:380, run_pathsim.py).
inline constexpr Decibels campaign_resample_gain{-12.0};

struct LaneChainConfig
{
    Decibels downsample_gain{campaign_resample_gain};
};

struct LaneChainReport
{
    PathsimRunReport pathsim{};     // the 8 kHz stage
    std::uint64_t input_samples{};  // 48 kHz samples read per pass
    std::uint64_t output_samples{}; // 48 kHz samples written
    std::uint64_t downsample_kernel_misses{};
    std::uint64_t upsample_kernel_misses{};
};

// 10^(dB / 20), computed at run time as the lane tool does (std::pow on the runtime).
[[nodiscard]] double amplitude_from_db(Decibels gain) noexcept;

class LaneChain
{
public:
    // source: 48 kHz mono, unit scale, rewindable. sink: 48 kHz mono PCM16. Both are
    // borrowed for the call. On failure the sink may hold part of the output.
    [[nodiscard]] m110::Result<LaneChainReport> run(const PathsimJob& job, const LaneChainConfig& config, UnitSource& source, Pcm16Sink& sink) noexcept;

private:
    PathsimDriver driver_{};
    LaneDownsampler downsampler_{};
    LaneUpsampler upsampler_{};
};

} // namespace channel_sim
