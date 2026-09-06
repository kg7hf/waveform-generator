#pragma once

#include "common/status.hpp"
#include "signal_lab/sample_stream.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace waveform_source
{

using Status = m110::Status;

// Sequential payload input; zero before its declared length is an I/O error.
class ByteSource
{
public:
    virtual ~ByteSource() = default;
    [[nodiscard]] virtual std::size_t read(std::uint8_t* bytes, std::size_t capacity) noexcept = 0;
};

// An encoder interprets its own opaque profile and produces canonical 48-kHz
// mono PCM16. configure() must provide the exact waveform frame count through
// total_frames(); artifact padding belongs to the generic WAV writer.
// Payload/source storage is caller-owned and must remain valid during the job.
class Encoder : public signal_lab::SampleSource
{
public:
    [[nodiscard]] virtual Status configure(std::string_view profile, ByteSource& payload, std::size_t payload_bytes) noexcept = 0;
    [[nodiscard]] virtual Status status() const noexcept = 0;
    virtual void stop() noexcept = 0;
};

} // namespace waveform_source
