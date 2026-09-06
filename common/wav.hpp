#pragma once

#include <cstdint>

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
};

struct WavReader
{
    void* context{};
    std::uint32_t file_size{};
    bool (*read)(void* context, std::uint32_t offset, std::uint8_t* destination,
                 std::uint32_t bytes) noexcept {};
};

constexpr std::uint32_t wav_fourcc(char a, char b, char c, char d) noexcept
{
    return static_cast<std::uint32_t>(static_cast<std::uint8_t>(a)) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(b)) << 8U) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(c)) << 16U) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(d)) << 24U);
}

constexpr std::uint16_t wav_read_le16(const std::uint8_t* bytes) noexcept
{
    return static_cast<std::uint16_t>(bytes[0]) |
           static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[1]) << 8U);
}

constexpr std::uint32_t wav_read_le32(const std::uint8_t* bytes) noexcept
{
    return static_cast<std::uint32_t>(bytes[0]) |
           (static_cast<std::uint32_t>(bytes[1]) << 8U) |
           (static_cast<std::uint32_t>(bytes[2]) << 16U) |
           (static_cast<std::uint32_t>(bytes[3]) << 24U);
}

inline bool wav_read_exact(const WavReader& reader, std::uint32_t offset,
                           std::uint8_t* destination, std::uint32_t bytes) noexcept
{
    if (reader.read == nullptr || destination == nullptr ||
        offset > reader.file_size || bytes > reader.file_size - offset)
    {
        return false;
    }
    return reader.read(reader.context, offset, destination, bytes);
}

inline WavValidationError validate_pcm16_mono_48k_wav(
    const WavReader& reader, WavPayload& payload) noexcept
{
    std::uint8_t riff_header[12U]{};
    std::uint8_t chunk_header[8U]{};
    std::uint8_t format_header[16U]{};

    if (reader.file_size < sizeof(riff_header) ||
        !wav_read_exact(reader, 0U, riff_header, sizeof(riff_header)))
    {
        return WavValidationError::io;
    }

    const auto riff_payload_bytes = wav_read_le32(riff_header + 4U);
    if (wav_read_le32(riff_header) != wav_fourcc('R', 'I', 'F', 'F') ||
        wav_read_le32(riff_header + 8U) != wav_fourcc('W', 'A', 'V', 'E') ||
        riff_payload_bytes < 4U ||
        riff_payload_bytes > UINT32_MAX - 8U ||
        riff_payload_bytes + 8U != reader.file_size)
    {
        return WavValidationError::invalid_riff;
    }

    const auto riff_end = riff_payload_bytes + 8U;
    std::uint32_t cursor = sizeof(riff_header);
    std::uint32_t header_bytes = sizeof(riff_header);
    bool have_format{};
    bool have_data{};
    std::uint16_t audio_format{};
    std::uint16_t channels{};
    std::uint32_t sample_rate{};
    std::uint32_t byte_rate{};
    std::uint16_t block_align{};
    std::uint16_t bits_per_sample{};

    while (cursor < riff_end)
    {
        if (riff_end - cursor < sizeof(chunk_header) ||
            header_bytes > wav_header_byte_limit - sizeof(chunk_header) ||
            !wav_read_exact(reader, cursor, chunk_header, sizeof(chunk_header)))
        {
            return WavValidationError::invalid_riff;
        }

        const auto chunk_id = wav_read_le32(chunk_header);
        const auto chunk_bytes = wav_read_le32(chunk_header + 4U);
        const auto data_offset = cursor + static_cast<std::uint32_t>(sizeof(chunk_header));
        if (chunk_bytes > riff_end - data_offset)
        {
            return WavValidationError::invalid_riff;
        }

        const auto padded_chunk_bytes = chunk_bytes + (chunk_bytes & 1U);
        if (padded_chunk_bytes < chunk_bytes ||
            padded_chunk_bytes > riff_end - data_offset)
        {
            return WavValidationError::invalid_riff;
        }
        const auto next_chunk = data_offset + padded_chunk_bytes;

        header_bytes += static_cast<std::uint32_t>(sizeof(chunk_header));
        if (chunk_id != wav_fourcc('d', 'a', 't', 'a'))
        {
            if (padded_chunk_bytes > wav_header_byte_limit - header_bytes)
            {
                return WavValidationError::invalid_riff;
            }
            header_bytes += padded_chunk_bytes;
        }

        if (chunk_id == wav_fourcc('f', 'm', 't', ' '))
        {
            if (have_format)
            {
                return WavValidationError::invalid_riff;
            }
            if (chunk_bytes < sizeof(format_header))
            {
                return WavValidationError::unsupported_format;
            }
            if (!wav_read_exact(reader, data_offset, format_header,
                                sizeof(format_header)))
            {
                return WavValidationError::io;
            }
            audio_format = wav_read_le16(format_header);
            channels = wav_read_le16(format_header + 2U);
            sample_rate = wav_read_le32(format_header + 4U);
            byte_rate = wav_read_le32(format_header + 8U);
            block_align = wav_read_le16(format_header + 12U);
            bits_per_sample = wav_read_le16(format_header + 14U);
            have_format = true;
        }
        else if (chunk_id == wav_fourcc('d', 'a', 't', 'a'))
        {
            if (have_data)
            {
                return WavValidationError::invalid_riff;
            }
            payload.offset = data_offset;
            payload.bytes = chunk_bytes;
            have_data = true;
        }

        cursor = next_chunk;
    }

    if (!have_format)
    {
        return WavValidationError::missing_format;
    }
    if (!have_data || payload.bytes == 0U)
    {
        return WavValidationError::missing_data;
    }
    if (audio_format != 1U || channels != 1U || sample_rate != 48000U ||
        byte_rate != 96000U || block_align != 2U || bits_per_sample != 16U ||
        (payload.bytes % sizeof(std::int16_t)) != 0U)
    {
        return WavValidationError::unsupported_format;
    }

    return WavValidationError::none;
}

} // namespace waveform_generator
