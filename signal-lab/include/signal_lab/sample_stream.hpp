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
    // Fill up to `capacity` PCM16 frames; returns the count (0 at end of stream).
    [[nodiscard]] virtual std::size_t read(std::int16_t* frames, std::size_t capacity) noexcept = 0;
    // Total frames when known in advance, else 0.
    [[nodiscard]] virtual std::uint64_t total_frames() const noexcept = 0;
};

class SampleSink
{
public:
    virtual ~SampleSink() = default;
    // Accept `count` PCM16 frames; returns false when the sink cannot take them.
    [[nodiscard]] virtual bool write(const std::int16_t* frames, std::size_t count) noexcept = 0;
};

// Pump the whole source through the engine into the sink using the engine's
// block size. Returns false if the sink refused data or the engine is unconfigured/failed.
[[nodiscard]] bool run_pipeline(SampleSource& source, Engine& engine, SampleSink& sink, std::int16_t* input_block, std::int16_t* output_block) noexcept;

} // namespace signal_lab
