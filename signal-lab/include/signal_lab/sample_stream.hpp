// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

#pragma once

// Source / sink abstraction around the engine so the same pipeline can be fed
// by a WAV file, an SD-card reader, or (later) a native waveform generator, and
// drained into a WAV writer, the RT1170 codec ring, or a future FPGA.
// The engine itself never learns what produced the samples.

#include "signal_lab/mixer.hpp"

#include <cstddef>
#include <cstdint>

namespace signal_lab
{

class SampleSource
{
public:
    virtual ~SampleSource() = default;
    // Fill up to `capacity` PCM16 frames: the count, 0 only at the end of the
    // stream; or why the next frames cannot be read, which is never the end.
    [[nodiscard]] virtual m110::Result<std::size_t> read(std::int16_t* frames, std::size_t capacity) noexcept = 0;
    // Total frames when known in advance, else 0.
    [[nodiscard]] virtual std::uint64_t total_frames() const noexcept = 0;

protected:
    // Only an implementation copies or moves itself, never through this interface (C.67).
    SampleSource() = default;
    SampleSource(const SampleSource&) = default;
    SampleSource& operator=(const SampleSource&) = default;
    SampleSource(SampleSource&&) = default;
    SampleSource& operator=(SampleSource&&) = default;
};

class SampleSink
{
public:
    virtual ~SampleSink() = default;
    // Accept `count` PCM16 frames: success, or why the sink cannot take them.
    [[nodiscard]] virtual m110::Status write(const std::int16_t* frames, std::size_t count) noexcept = 0;

protected:
    // Only an implementation copies or moves itself, never through this interface (C.67).
    SampleSink() = default;
    SampleSink(const SampleSink&) = default;
    SampleSink& operator=(const SampleSink&) = default;
    SampleSink(SampleSink&&) = default;
    SampleSink& operator=(SampleSink&&) = default;
};

// Streams `source` through `engine` into `sink`, a block of the engine's size at
// a time, until the source ends: success, the source's failure, the engine's
// (its latched Status, also one from before the call), the sink's, or
// invalid_argument for a missing block or an unconfigured engine.
[[nodiscard]] m110::Status run_pipeline(SampleSource& source, Engine& engine, SampleSink& sink, std::int16_t* input_block, std::int16_t* output_block) noexcept;

} // namespace signal_lab
