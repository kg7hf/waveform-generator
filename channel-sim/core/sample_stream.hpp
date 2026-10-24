// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// Streaming sample interfaces of the channel-sim chain (guide: docs/modules/pathsim.md).
//
// The chain core knows no file format. The host CLI wraps WAV files in these
// interfaces (cli/wav_file.hpp). pathsim's level seed needs
// the whole input before the first block, so a source can be rewound and read again:
// the second pass must deliver exactly the samples of the first.

#pragma once

#include "common/status.hpp"
#include "core/pcm.hpp"

#include <cstddef>
#include <span>

namespace channel_sim
{

template <typename Sample> class SampleSource
{
public:
    virtual ~SampleSource() = default;
    SampleSource(const SampleSource&) = delete;
    SampleSource& operator=(const SampleSource&) = delete;
    SampleSource(SampleSource&&) = delete;
    SampleSource& operator=(SampleSource&&) = delete;

    // Reads up to destination.size() samples and returns how many were written.
    // 0 means the end of the stream; fewer than requested does not.
    [[nodiscard]] virtual m110::Result<std::size_t> read(std::span<Sample> destination) noexcept = 0;

    // Starts the stream again from its first sample.
    [[nodiscard]] virtual m110::Status rewind() noexcept = 0;

protected:
    SampleSource() = default;
};

template <typename Sample> class SampleSink
{
public:
    virtual ~SampleSink() = default;
    SampleSink(const SampleSink&) = delete;
    SampleSink& operator=(const SampleSink&) = delete;
    SampleSink(SampleSink&&) = delete;
    SampleSink& operator=(SampleSink&&) = delete;

    // Accepts every sample or fails; samples arrive in stream order.
    [[nodiscard]] virtual m110::Status write(std::span<const Sample> samples) noexcept = 0;

protected:
    SampleSink() = default;
};

// PCM16 mono streams (pathsim's 8 kHz input and output, the chain's 48 kHz output).
using Pcm16Source = SampleSource<Pcm16>;
using Pcm16Sink = SampleSink<Pcm16>;
// Unit-scaled mono stream (full scale = 1.0): the chain's 48 kHz input, which the lane
// resampler accepts as PCM16, PCM24 or float32.
using UnitSource = SampleSource<double>;

} // namespace channel_sim
