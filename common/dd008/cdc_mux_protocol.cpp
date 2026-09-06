#include "common/dd008/cdc_mux_protocol.hpp"

#include <algorithm>

namespace m110
{
namespace
{

constexpr std::byte magic_0{0x4DU};
constexpr std::byte magic_1{0x31U};

std::uint16_t read_u16(std::span<const std::byte> bytes,
                       std::size_t offset) noexcept
{
    return static_cast<std::uint16_t>(
        std::to_integer<std::uint16_t>(bytes[offset]) |
        (std::to_integer<std::uint16_t>(bytes[offset + 1U]) << 8U));
}

std::uint32_t read_u32(std::span<const std::byte> bytes,
                       std::size_t offset) noexcept
{
    return std::to_integer<std::uint32_t>(bytes[offset]) |
           (std::to_integer<std::uint32_t>(bytes[offset + 1U]) << 8U) |
           (std::to_integer<std::uint32_t>(bytes[offset + 2U]) << 16U) |
           (std::to_integer<std::uint32_t>(bytes[offset + 3U]) << 24U);
}

void write_u16(std::span<std::byte> bytes, std::size_t offset,
               std::uint16_t value) noexcept
{
    bytes[offset] = static_cast<std::byte>(value & 0xFFU);
    bytes[offset + 1U] = static_cast<std::byte>((value >> 8U) & 0xFFU);
}

void write_u32(std::span<std::byte> bytes, std::size_t offset,
               std::uint32_t value) noexcept
{
    bytes[offset] = static_cast<std::byte>(value & 0xFFU);
    bytes[offset + 1U] = static_cast<std::byte>((value >> 8U) & 0xFFU);
    bytes[offset + 2U] = static_cast<std::byte>((value >> 16U) & 0xFFU);
    bytes[offset + 3U] = static_cast<std::byte>((value >> 24U) & 0xFFU);
}

bool known_type(CdcMuxFrameType type) noexcept
{
    switch (type)
    {
    case CdcMuxFrameType::hello:
    case CdcMuxFrameType::hello_ack:
    case CdcMuxFrameType::channel_open:
    case CdcMuxFrameType::channel_close:
    case CdcMuxFrameType::stream_data:
    case CdcMuxFrameType::reset:
    case CdcMuxFrameType::error:
        return true;
    }

    return false;
}

bool known_channel(CdcMuxChannel channel) noexcept
{
    switch (channel)
    {
    case CdcMuxChannel::link:
    case CdcMuxChannel::control_status:
    case CdcMuxChannel::binary_data:
    case CdcMuxChannel::provisioning:
    case CdcMuxChannel::diagnostic:
        return true;
    }

    return false;
}

bool valid_type_channel(CdcMuxFrameType type, CdcMuxChannel channel,
                        std::size_t payload_size) noexcept
{
    switch (type)
    {
    case CdcMuxFrameType::hello:
    case CdcMuxFrameType::hello_ack:
        return channel == CdcMuxChannel::link && payload_size == 8U;

    case CdcMuxFrameType::channel_open:
    case CdcMuxFrameType::channel_close:
        return channel != CdcMuxChannel::link && payload_size == 0U;

    case CdcMuxFrameType::stream_data:
        return channel != CdcMuxChannel::link;

    case CdcMuxFrameType::reset:
        return channel == CdcMuxChannel::link && payload_size == 0U;

    case CdcMuxFrameType::error:
        return channel == CdcMuxChannel::link && payload_size >= 2U;
    }

    return false;
}

Result<std::size_t> cobs_encode(std::span<const std::byte> input,
                                std::span<std::byte> output) noexcept
{
    if (output.empty())
    {
        return Status{StatusCode::buffer_too_small, "COBS output buffer is empty"};
    }

    std::size_t read_index = 0U;
    std::size_t write_index = 1U;
    std::size_t code_index = 0U;
    std::uint8_t code = 1U;

    while (read_index < input.size())
    {
        const auto value = input[read_index++];

        if (value == std::byte{0U})
        {
            if (code_index >= output.size())
            {
                return Status{StatusCode::buffer_too_small, "COBS output buffer is too small"};
            }

            output[code_index] = static_cast<std::byte>(code);
            code_index = write_index;
            ++write_index;
            code = 1U;
        }
        else
        {
            if (write_index >= output.size())
            {
                return Status{StatusCode::buffer_too_small, "COBS output buffer is too small"};
            }

            output[write_index++] = value;
            ++code;

            if (code == 0xFFU)
            {
                output[code_index] = static_cast<std::byte>(code);
                code_index = write_index;
                ++write_index;
                code = 1U;
            }
        }
    }

    if (code_index >= output.size())
    {
        return Status{StatusCode::buffer_too_small, "COBS output buffer is too small"};
    }

    output[code_index] = static_cast<std::byte>(code);
    return write_index;
}

Result<std::size_t> cobs_decode(std::span<const std::byte> input,
                                std::span<std::byte> output) noexcept
{
    std::size_t read_index = 0U;
    std::size_t write_index = 0U;

    while (read_index < input.size())
    {
        const auto code = std::to_integer<std::uint8_t>(input[read_index++]);

        if (code == 0U)
        {
            return Status{StatusCode::invalid_argument, "zero code in COBS packet"};
        }

        const std::size_t copy_size = static_cast<std::size_t>(code - 1U);

        if (copy_size > input.size() - read_index ||
                copy_size > output.size() - write_index)
        {
            return Status{StatusCode::invalid_argument, "truncated or oversized COBS packet"};
        }

        std::copy_n(input.begin() + static_cast<std::ptrdiff_t>(read_index),
                    static_cast<std::ptrdiff_t>(copy_size),
                    output.begin() + static_cast<std::ptrdiff_t>(write_index));
        read_index += copy_size;
        write_index += copy_size;

        if (code != 0xFFU && read_index < input.size())
        {
            if (write_index >= output.size())
            {
                return Status{StatusCode::buffer_too_small, "decoded COBS packet is too large"};
            }

            output[write_index++] = std::byte{0U};
        }
    }

    return write_index;
}

CdcMuxDecodedPacket parse_frame(std::span<const std::byte> decoded) noexcept
{
    if (decoded.size() < cdc_mux_header_size + cdc_mux_crc_size)
    {
        return {CdcMuxDecodeError::frame_too_short, {}};
    }

    if (decoded[0] != magic_0 || decoded[1] != magic_1)
    {
        return {CdcMuxDecodeError::bad_magic, {}};
    }

    if (std::to_integer<std::uint8_t>(decoded[2]) != cdc_mux_protocol_version)
    {
        return {CdcMuxDecodeError::unsupported_version, {}};
    }

    if (decoded[5] != std::byte{0U})
    {
        return {CdcMuxDecodeError::unsupported_flags, {}};
    }

    const auto payload_size = static_cast<std::size_t>(read_u16(decoded, 6U));

    if (payload_size > cdc_mux_maximum_payload_size ||
            decoded.size() != cdc_mux_header_size + payload_size + cdc_mux_crc_size)
    {
        return {CdcMuxDecodeError::length_mismatch, {}};
    }

    const auto crc_offset = cdc_mux_header_size + payload_size;
    const auto expected_crc = read_u32(decoded, crc_offset);
    const auto actual_crc = cdc_mux_crc32c(decoded.first(crc_offset));

    if (expected_crc != actual_crc)
    {
        return {CdcMuxDecodeError::bad_crc, {}};
    }

    const auto type = static_cast<CdcMuxFrameType>(
        std::to_integer<std::uint8_t>(decoded[3]));
    const auto channel = static_cast<CdcMuxChannel>(
        std::to_integer<std::uint8_t>(decoded[4]));

    if (!known_type(type))
    {
        return {CdcMuxDecodeError::unsupported_type, {}};
    }

    if (!known_channel(channel))
    {
        return {CdcMuxDecodeError::unsupported_channel, {}};
    }

    if (!valid_type_channel(type, channel, payload_size))
    {
        return {CdcMuxDecodeError::invalid_type_channel, {}};
    }

    return {
        CdcMuxDecodeError::none,
        {
            type,
            channel,
            read_u32(decoded, 8U),
            decoded.subspan(cdc_mux_header_size, payload_size),
        },
    };
}

} // namespace

std::uint32_t cdc_mux_crc32c(std::span<const std::byte> bytes) noexcept
{
    std::uint32_t crc = 0xFFFFFFFFU;

    for (const auto byte : bytes)
    {
        crc ^= std::to_integer<std::uint8_t>(byte);

        for (std::uint32_t bit = 0U; bit < 8U; ++bit)
        {
            const std::uint32_t mask = 0U - (crc & 1U);
            crc = (crc >> 1U) ^ (0x82F63B78U & mask);
        }
    }

    return crc ^ 0xFFFFFFFFU;
}

Result<std::size_t> encode_cdc_mux_frame(const CdcMuxFrameView& frame,
                                         std::span<std::byte> output) noexcept
{
    if (!known_type(frame.type) || !known_channel(frame.channel) ||
            !valid_type_channel(frame.type, frame.channel, frame.payload.size()))
    {
        return Status{StatusCode::invalid_argument,
                      "invalid DD-008 frame type, channel, or payload"};
    }

    if (frame.payload.size() > cdc_mux_maximum_payload_size)
    {
        return Status{StatusCode::invalid_argument, "DD-008 payload is too large"};
    }

    std::array<std::byte, cdc_mux_maximum_decoded_frame_size> decoded{};
    const auto decoded_size = cdc_mux_header_size + frame.payload.size() + cdc_mux_crc_size;
    const std::span<std::byte> decoded_frame{decoded.data(), decoded_size};
    decoded_frame[0] = magic_0;
    decoded_frame[1] = magic_1;
    decoded_frame[2] = static_cast<std::byte>(cdc_mux_protocol_version);
    decoded_frame[3] = static_cast<std::byte>(frame.type);
    decoded_frame[4] = static_cast<std::byte>(frame.channel);
    decoded_frame[5] = std::byte{0U};
    write_u16(decoded_frame, 6U, static_cast<std::uint16_t>(frame.payload.size()));
    write_u32(decoded_frame, 8U, frame.sequence);
    std::copy(frame.payload.begin(), frame.payload.end(),
              decoded_frame.begin() + static_cast<std::ptrdiff_t>(cdc_mux_header_size));
    write_u32(decoded_frame, cdc_mux_header_size + frame.payload.size(),
              cdc_mux_crc32c(decoded_frame.first(cdc_mux_header_size + frame.payload.size())));

    if (output.size() < 2U)
    {
        return Status{StatusCode::buffer_too_small, "DD-008 wire output is too small"};
    }

    const auto encoded_result = cobs_encode(decoded_frame, output.first(output.size() - 1U));

    if (!encoded_result)
    {
        return encoded_result.status();
    }

    const auto encoded_size = encoded_result.value();

    if (encoded_size >= output.size())
    {
        return Status{StatusCode::buffer_too_small, "DD-008 wire output lacks delimiter space"};
    }

    output[encoded_size] = std::byte{0U};
    return encoded_size + 1U;
}

Status CdcMuxStreamDecoder::ingest(std::span<const std::byte> bytes,
                                   CdcMuxPacketCallback callback,
                                   void* callback_context) noexcept
{
    if (callback == nullptr)
    {
        return {StatusCode::invalid_argument, "DD-008 decoder callback is null"};
    }

    for (const auto byte : bytes)
    {
        if (byte == std::byte{0U})
        {
            const auto status = finish_packet(callback, callback_context);

            if (!status.is_ok())
            {
                return status;
            }

            continue;
        }

        if (discarding_oversized_packet_)
        {
            continue;
        }

        if (encoded_size_ == encoded_.size())
        {
            discarding_oversized_packet_ = true;
            encoded_size_ = 0U;
            continue;
        }

        encoded_[encoded_size_++] = byte;
    }

    return Status::success();
}

void CdcMuxStreamDecoder::reset() noexcept
{
    encoded_size_ = 0U;
    discarding_oversized_packet_ = false;
}

Status CdcMuxStreamDecoder::finish_packet(CdcMuxPacketCallback callback,
                                          void* callback_context) noexcept
{
    if (discarding_oversized_packet_)
    {
        discarding_oversized_packet_ = false;
        encoded_size_ = 0U;
        ++statistics_.rejected_packets;
        ++statistics_.oversized_packets;
        return callback(callback_context,
                        CdcMuxDecodedPacket{CdcMuxDecodeError::packet_too_large, {}});
    }

    if (encoded_size_ == 0U)
    {
        ++statistics_.empty_packets;
        return Status::success();
    }

    const auto decoded_result = cobs_decode(
        std::span<const std::byte>{encoded_.data(), encoded_size_}, decoded_);
    encoded_size_ = 0U;

    if (!decoded_result)
    {
        ++statistics_.rejected_packets;
        return callback(callback_context,
                        CdcMuxDecodedPacket{CdcMuxDecodeError::malformed_cobs, {}});
    }

    const auto packet = parse_frame(
        std::span<const std::byte>{decoded_.data(), decoded_result.value()});

    if (packet.valid())
    {
        ++statistics_.valid_frames;
    }
    else
    {
        ++statistics_.rejected_packets;
    }

    return callback(callback_context, packet);
}

} // namespace m110
