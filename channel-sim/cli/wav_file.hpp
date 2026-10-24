// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// Host WAV files as channel-sim streams (guide: docs/modules/pathsim.md).
//
// Reading: RIFF/WAVE with a "fmt " chunk before "data"; PCM16 and PCM24 (format 1)
// and IEEE float32 (format 3), 1 or 2 channels. Chunk sizes (with their pad byte)
// are checked in 64 bits against the file's length, so a malformed size is refused
// rather than misparsed. Channel 0 is streamed, as pathsim
// processes channel 0 and the lane resampler accepts mono only. Samples convert as
// the lane tools convert them: s / 32768 (PCM16), s / 8388608 (PCM24), float as is.
// Writing: the 44-byte canonical PCM16 header pathsim (AudioFile.h) and the lane
// resampler both write, with every sample repeated on each channel (pathsim copies
// channel 0 to the others). The header sizes are written by finish().
//
// Host only: file streams (whose buffers the C++ library allocates), no exceptions.

#pragma once

#include "common/status.hpp"
#include "core/pcm.hpp"
#include "core/sample_stream.hpp"
#include "core/units.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>

namespace channel_sim::cli
{

enum class WavEncoding : std::uint8_t
{
    pcm16,
    pcm24,
    float32
};

struct WavFormat
{
    SampleRate rate{};
    std::uint16_t channels{};
    WavEncoding encoding{WavEncoding::pcm16};
    std::uint16_t bits{};
    std::uint16_t frame_bytes{};
    std::uint64_t frames{};
};

// Streams channel 0 of a WAV file, as PCM16 (pathsim's 8 kHz input) or unit samples
// (the chain's 48 kHz input).
class WavFileSource final : public Pcm16Source
{
public:
    [[nodiscard]] m110::Status open(const char* path) noexcept;
    [[nodiscard]] const WavFormat& format() const noexcept;

    // PCM16 files only.
    [[nodiscard]] m110::Result<std::size_t> read(std::span<Pcm16> destination) noexcept override;
    [[nodiscard]] m110::Status rewind() noexcept override;
    // Any accepted encoding.
    [[nodiscard]] m110::Result<std::size_t> read_unit(std::span<double> destination) noexcept;

private:
    static constexpr std::size_t buffer_bytes = 32768U;

    [[nodiscard]] m110::Status find_data() noexcept;
    [[nodiscard]] m110::Status check_inside(std::uint64_t bytes, const char* beyond) noexcept;
    [[nodiscard]] m110::Status read_format(std::uint32_t chunk_bytes) noexcept;
    [[nodiscard]] m110::Status accept_data(std::uint32_t chunk_bytes) noexcept;
    [[nodiscard]] m110::Result<std::size_t> read_frames(std::size_t wanted) noexcept;

    std::ifstream file_{};
    std::uint64_t file_bytes_{};
    std::streampos data_start_{};
    WavFormat format_{};
    std::uint16_t format_tag_{};
    bool have_format_{};
    std::uint64_t frames_left_{};
    std::array<char, buffer_bytes> buffer_{};
};

// A WavFileSource seen as the chain's unit-scaled input.
class WavUnitSource final : public UnitSource
{
public:
    explicit WavUnitSource(WavFileSource& file) noexcept;

    [[nodiscard]] m110::Result<std::size_t> read(std::span<double> destination) noexcept override;
    [[nodiscard]] m110::Status rewind() noexcept override;

private:
    WavFileSource* file_;
};

// Writes PCM16 samples as a canonical WAV, each sample on every channel.
class WavFileSink final : public Pcm16Sink
{
public:
    [[nodiscard]] m110::Status open(const char* path, SampleRate rate, std::uint16_t channels) noexcept;
    [[nodiscard]] m110::Status write(std::span<const Pcm16> samples) noexcept override;
    // Flushes, writes the final RIFF and data sizes and closes. Required for a valid file.
    [[nodiscard]] m110::Status finish() noexcept;

private:
    static constexpr std::size_t buffer_bytes = 65536U;

    [[nodiscard]] m110::Status write_header(std::uint32_t data_bytes) noexcept;
    [[nodiscard]] m110::Status flush() noexcept;

    std::ofstream file_{};
    SampleRate rate_{};
    std::uint16_t channels_{};
    std::uint64_t frames_{};
    std::array<char, buffer_bytes> buffer_{};
    std::size_t buffered_{};
};

} // namespace channel_sim::cli
