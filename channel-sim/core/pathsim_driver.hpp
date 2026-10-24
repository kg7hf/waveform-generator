// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// The 8 kHz pathsim job: the patched pathsim main.cpp's orchestration, streamed
// (guide: docs/modules/pathsim.md).
//
// run() calls pathsim's own classes in main.cpp's order, so the random draws and
// every floating-point operation happen in the same sequence:
//   pass 1  read the whole input; RMS of the samples with |v| > 1e-4 (v = s / 32768)
//   rewind  the source
//   seed    srand(seed) -> seed_pathsim_rng(seed)
//   init    PathSimProcessor::init(params)       (draws: Rayleigh filter preload)
//   truth   enable_truth_capture(path, seed)     (optional, host diagnostic)
//   level   seed_signal_rms(rms) if any sample was active
//   warm-up warm_up_fading(4 s)                  (draws)
//   pass 2  2048-sample blocks, the last one zero-padded; process_buffer (draws);
//           every input sample's output written as trunc(clamp(y) * 32767)
// The output has exactly as many samples as the input.
//
// Memory: the driver's own buffers are fixed arrays (20 KB). pathsim's processor
// (about 40 KB in place) allocates from the global heap: its path buffers and
// parameter copies in init(), and a small vector in every process_buffer().
// pathsim is the host reference oracle only (the firmware never contains it), so
// that is the ordinary heap. The truth path is copied into a std::string for
// pathsim's API.

#pragma once

#include "common/status.hpp"
#include "core/channel_spec.hpp"
#include "core/pcm.hpp"
#include "core/sample_stream.hpp"
#include "core/units.hpp"

#include <PathSimProcessor.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>

namespace channel_sim
{

struct PathsimJob
{
    ChannelSpec channel{};
    Seed seed{};
    // pathsim's --truth-out (M110PTr1 channel-truth export). Empty: none. pathsim
    // opens the file itself and ignores an open failure; the caller checks the file.
    std::string_view truth_path{};
};

struct PathsimRunReport
{
    std::uint64_t samples{};        // input samples = output samples
    std::uint64_t active_samples{}; // samples with |v| > 1e-4 in pass 1
    double seed_rms{};              // level-tracker seed; 0 when no sample was active (tracker starts at 0.122)
    std::uint64_t blocks{};         // 2048-sample blocks processed
};

class PathsimDriver
{
public:
    // pathsim's block (PathSimProcessor::BUF_SIZE).
    static constexpr std::size_t block_samples = 2048U;
    // main.cpp's file-mode constants.
    static constexpr double active_threshold = 1e-4;
    static constexpr Seconds fading_warm_up{4.0};

    // Runs one job from source to sink (both 8 kHz PCM16 mono). On failure the sink
    // may have received part of the output.
    [[nodiscard]] m110::Result<PathsimRunReport> run(const PathsimJob& job, Pcm16Source& source, Pcm16Sink& sink) noexcept;

private:
    struct ActiveLevel
    {
        std::uint64_t samples{};
        std::uint64_t active{};
        double sum_squares{};
    };

    [[nodiscard]] m110::Result<ActiveLevel> measure_active_level(Pcm16Source& source) noexcept;
    // Fills pcm_ from the source until it is full or the stream ends.
    [[nodiscard]] m110::Result<std::size_t> fill_block(Pcm16Source& source) noexcept;
    // Seeds the generator and readies a fresh processor (main.cpp's steps up to the warm-up).
    [[nodiscard]] PathSim::PathSimProcessor& start_processor(const PathsimJob& job, const ActiveLevel& level) noexcept;
    [[nodiscard]] m110::Result<std::uint64_t> process_blocks(PathSim::PathSimProcessor& processor, Pcm16Source& source, Pcm16Sink& sink, std::uint64_t expected) noexcept;

    std::optional<PathSim::PathSimProcessor> processor_{};
    std::array<double, block_samples> block_{};
    std::array<Pcm16, block_samples> pcm_{};
};

static_assert(std::cmp_equal(PathsimDriver::block_samples, PathSim::PathSimProcessor::BUF_SIZE), "the driver's block must be pathsim's");

} // namespace channel_sim
