#pragma once

#include "common/status.hpp"
#include "signal_lab/sample_stream.hpp"

#include <cstddef>
#include <cstdint>
#include <algorithm>
#include <array>
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
// mono normalized float. The inherited PCM16 reader is retained as a legacy
// adapter for existing tests and imported producers. configure() must provide
// the exact waveform frame count through
// total_frames(); artifact padding belongs to the generic WAV writer.
// Payload/source storage is caller-owned and must remain valid during the job.
class Encoder : public signal_lab::SampleSource
{
public:
    [[nodiscard]] virtual Status configure(std::string_view profile, ByteSource& payload, std::size_t payload_bytes) noexcept = 0;
    [[nodiscard]] virtual Status status() const noexcept = 0;
    virtual void stop() noexcept = 0;

    // Production source boundary. Legacy encoders inherit a bounded PCM16-to-
    // float adapter; native encoders override this to avoid losing source bits.
    [[nodiscard]] virtual std::size_t read_float(float* frames,
                                                 std::size_t capacity) noexcept
    {
        if (frames == nullptr || capacity == 0U)
        {
            return 0U;
        }
        std::array<std::int16_t, 64U> legacy{};
        const auto request = std::min(capacity, legacy.size());
        const auto count = read(legacy.data(), request);
        for (std::size_t index = 0U; index < count; ++index)
        {
            frames[index] = static_cast<float>(legacy[index]) / 32768.0F;
        }
        return count;
    }
};

} // namespace waveform_source
