// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

#pragma once

#include "common/status.hpp"

#include <cstdint>
#include <span>

namespace waveform_generator
{

inline constexpr std::uint32_t wav_header_byte_limit = 65536U;

enum class WavValidationError : std::uint32_t
{
    none = 0U,
    io = 1U,
    invalid_riff = 2U,
    missing_format = 3U,
    missing_data = 4U,
    unsupported_format = 5U,
};

struct WavPayload
{
    std::uint32_t offset{};
    std::uint32_t bytes{};
    std::uint16_t bits_per_sample{};
    std::uint16_t bytes_per_frame{};
};

// A read of WAV bytes that would go past the end of the file.
inline constexpr m110::Status wav_read_past_end{.code = m110::StatusCode::invalid_argument, .message = "a WAV read past the end of the file"};
// A WAV read without a reader or a destination.
inline constexpr m110::Status wav_read_unready{.code = m110::StatusCode::invalid_argument, .message = "a WAV read without a reader or a destination"};

// The file a WAV is validated from: its size, and a callback that reads
// exactly `bytes` at `offset` (inside the file) into `destination` and says
// whether it did: success, or its failure.
struct WavReader
{
    void* context{};
    std::uint32_t file_size{};
    m110::Status (*read)(void* context, std::uint32_t offset, std::uint8_t* destination, std::uint32_t bytes) noexcept {};
};

constexpr std::uint32_t wav_fourcc(char a, char b, char c, char d) noexcept
{
    return static_cast<std::uint32_t>(static_cast<std::uint8_t>(a)) | (static_cast<std::uint32_t>(static_cast<std::uint8_t>(b)) << 8U) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(c)) << 16U) | (static_cast<std::uint32_t>(static_cast<std::uint8_t>(d)) << 24U);
}

constexpr std::uint16_t wav_read_le16(const std::uint8_t* bytes) noexcept
{
    return static_cast<std::uint16_t>(bytes[0]) | static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[1]) << 8U);
}

constexpr std::uint32_t wav_read_le32(const std::uint8_t* bytes) noexcept
{
    return static_cast<std::uint32_t>(bytes[0]) | (static_cast<std::uint32_t>(bytes[1]) << 8U) | (static_cast<std::uint32_t>(bytes[2]) << 16U) | (static_cast<std::uint32_t>(bytes[3]) << 24U);
}

// Fills `destination` with the bytes at `offset`: success; wav_read_past_end
// for bytes beyond the file's end, which the callback is not asked for; the
// callback's own failure; or wav_read_unready.
[[nodiscard]] inline m110::Status wav_read_exact(const WavReader& reader, std::uint32_t offset, std::span<std::uint8_t> destination) noexcept
{
    if (reader.read == nullptr || destination.data() == nullptr)
    {
        return wav_read_unready;
    }

    // offset + size could wrap in 32 bits; compare what is left instead.
    if (offset > reader.file_size || destination.size() > reader.file_size - offset)
    {
        return wav_read_past_end;
    }

    return reader.read(reader.context, offset, destination.data(), static_cast<std::uint32_t>(destination.size()));
}

namespace wav_detail
{

struct Format
{
    std::uint16_t audio_format{};
    std::uint16_t channels{};
    std::uint32_t sample_rate{};
    std::uint32_t byte_rate{};
    std::uint16_t block_align{};
    std::uint16_t bits_per_sample{};
    bool have_format{};
    bool have_data{};
};

struct RiffState
{
    std::uint32_t end{};
    std::uint32_t cursor{12U};
    std::uint32_t header_bytes{12U};
};

struct Chunk
{
    std::uint32_t id{};
    std::uint32_t offset{};
    std::uint32_t bytes{};
    std::uint32_t next{};
};

[[nodiscard]] inline WavValidationError read_riff_header(const WavReader& reader, RiffState& state) noexcept
{
    std::uint8_t header[12U]{};

    if (reader.file_size < sizeof(header) || !wav_read_exact(reader, 0U, header).is_ok())
    {
        return WavValidationError::io;
    }

    const std::uint32_t riff_bytes = wav_read_le32(header + 4U);

    if (wav_read_le32(header) != wav_fourcc('R', 'I', 'F', 'F') || wav_read_le32(header + 8U) != wav_fourcc('W', 'A', 'V', 'E') || riff_bytes < 4U || riff_bytes > UINT32_MAX - 8U ||
        riff_bytes + 8U != reader.file_size)
    {
        return WavValidationError::invalid_riff;
    }

    state.end = riff_bytes + 8U;
    return WavValidationError::none;
}

[[nodiscard]] inline WavValidationError read_chunk(const WavReader& reader, RiffState& state, Chunk& chunk) noexcept
{
    std::uint8_t header[8U]{};

    if (state.end - state.cursor < sizeof(header) || state.header_bytes > wav_header_byte_limit - sizeof(header) || !wav_read_exact(reader, state.cursor, header).is_ok())
    {
        return WavValidationError::invalid_riff;
    }

    chunk.id = wav_read_le32(header);
    chunk.bytes = wav_read_le32(header + 4U);
    chunk.offset = state.cursor + static_cast<std::uint32_t>(sizeof(header));

    if (chunk.bytes > state.end - chunk.offset)
    {
        return WavValidationError::invalid_riff;
    }

    const std::uint32_t padded_bytes = chunk.bytes + (chunk.bytes & 1U);

    if (padded_bytes < chunk.bytes || padded_bytes > state.end - chunk.offset)
    {
        return WavValidationError::invalid_riff;
    }

    chunk.next = chunk.offset + padded_bytes;
    state.header_bytes += static_cast<std::uint32_t>(sizeof(header));

    if (chunk.id != wav_fourcc('d', 'a', 't', 'a'))
    {
        if (padded_bytes > wav_header_byte_limit - state.header_bytes)
        {
            return WavValidationError::invalid_riff;
        }

        state.header_bytes += padded_bytes;
    }

    return WavValidationError::none;
}

[[nodiscard]] inline WavValidationError read_format(const WavReader& reader, const Chunk& chunk, Format& format) noexcept
{
    if (format.have_format)
    {
        return WavValidationError::invalid_riff;
    }

    if (chunk.bytes < 16U)
    {
        return WavValidationError::unsupported_format;
    }

    std::uint8_t header[16U]{};

    if (!wav_read_exact(reader, chunk.offset, header).is_ok())
    {
        return WavValidationError::io;
    }

    format.audio_format = wav_read_le16(header);
    format.channels = wav_read_le16(header + 2U);
    format.sample_rate = wav_read_le32(header + 4U);
    format.byte_rate = wav_read_le32(header + 8U);
    format.block_align = wav_read_le16(header + 12U);
    format.bits_per_sample = wav_read_le16(header + 14U);
    format.have_format = true;
    return WavValidationError::none;
}

[[nodiscard]] inline WavValidationError parse_riff(const WavReader& reader, WavPayload& payload, Format& format) noexcept
{
    RiffState state{};

    if (const WavValidationError header = read_riff_header(reader, state); header != WavValidationError::none)
    {
        return header;
    }

    while (state.cursor < state.end)
    {
        Chunk chunk{};

        if (const WavValidationError read = read_chunk(reader, state, chunk); read != WavValidationError::none)
        {
            return read;
        }

        if (chunk.id == wav_fourcc('f', 'm', 't', ' '))
        {
            if (const WavValidationError read = read_format(reader, chunk, format); read != WavValidationError::none)
            {
                return read;
            }
        }
        else if (chunk.id == wav_fourcc('d', 'a', 't', 'a'))
        {
            if (format.have_data)
            {
                return WavValidationError::invalid_riff;
            }

            payload.offset = chunk.offset;
            payload.bytes = chunk.bytes;
            format.have_data = true;
        }

        state.cursor = chunk.next;
    }

    return WavValidationError::none;
}

[[nodiscard]] inline WavValidationError validate_pcm_wav(const WavReader& reader, WavPayload& payload, std::uint16_t required_channels, std::uint16_t required_bits_per_sample) noexcept
{
    Format format{};

    if (const WavValidationError parsed = parse_riff(reader, payload, format); parsed != WavValidationError::none)
    {
        return parsed;
    }

    if (!format.have_format)
    {
        return WavValidationError::missing_format;
    }

    if (!format.have_data || payload.bytes == 0U)
    {
        return WavValidationError::missing_data;
    }

    const std::uint16_t required_bytes_per_frame = static_cast<std::uint16_t>((required_bits_per_sample / 8U) * required_channels);

    if ((required_bits_per_sample != 16U && required_bits_per_sample != 24U) || (required_channels != 1U && required_channels != 2U) || (required_channels == 2U && required_bits_per_sample != 24U) ||
        format.audio_format != 1U || format.channels != required_channels || format.sample_rate != 48000U || format.byte_rate != 48000U * required_bytes_per_frame ||
        format.block_align != required_bytes_per_frame || format.bits_per_sample != required_bits_per_sample || (payload.bytes % required_bytes_per_frame) != 0U)
    {
        return WavValidationError::unsupported_format;
    }

    payload.bits_per_sample = format.bits_per_sample;
    payload.bytes_per_frame = format.block_align;
    return WavValidationError::none;
}

} // namespace wav_detail

inline WavValidationError validate_pcm_mono_48k_wav(const WavReader& reader, WavPayload& payload, std::uint16_t required_bits_per_sample) noexcept
{
    return wav_detail::validate_pcm_wav(reader, payload, 1U, required_bits_per_sample);
}

inline WavValidationError validate_pcm16_mono_48k_wav(const WavReader& reader, WavPayload& payload) noexcept
{
    return validate_pcm_mono_48k_wav(reader, payload, 16U);
}

inline WavValidationError validate_pcm24_mono_48k_wav(const WavReader& reader, WavPayload& payload) noexcept
{
    return validate_pcm_mono_48k_wav(reader, payload, 24U);
}

// Accepts interleaved stereo PCM24 at 48 kHz; bytes_per_frame is six and the
// payload reader remains responsible for preserving left/right byte order.
inline WavValidationError validate_pcm24_stereo_48k_wav(const WavReader& reader, WavPayload& payload) noexcept
{
    return wav_detail::validate_pcm_wav(reader, payload, 2U, 24U);
}

} // namespace waveform_generator
