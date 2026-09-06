#include "common/dd008/cdc_mux_session.hpp"

#include <algorithm>
#include <array>
#include <cstddef>

namespace m110
{
namespace
{

constexpr std::uint16_t error_bad_crc = 2U;
constexpr std::uint16_t error_unsupported_version = 3U;
constexpr std::uint16_t error_oversize = 4U;
constexpr std::uint16_t error_sequence = 5U;
constexpr std::uint16_t error_unsupported_type = 6U;
constexpr std::uint16_t error_unsupported_channel = 7U;
constexpr std::uint16_t error_channel_closed = 8U;
constexpr std::uint16_t error_service_unavailable = 10U;

std::size_t channel_index(CdcMuxChannel channel) noexcept
{
    return static_cast<std::size_t>(channel);
}

bool application_channel(CdcMuxChannel channel) noexcept
{
    return channel == CdcMuxChannel::control_status ||
           channel == CdcMuxChannel::binary_data;
}

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

std::uint16_t decode_error_code(CdcMuxDecodeError error) noexcept
{
    switch (error)
    {
    case CdcMuxDecodeError::bad_crc:
        return error_bad_crc;
    case CdcMuxDecodeError::unsupported_version:
        return error_unsupported_version;
    case CdcMuxDecodeError::packet_too_large:
    case CdcMuxDecodeError::length_mismatch:
        return error_oversize;
    case CdcMuxDecodeError::unsupported_type:
        return error_unsupported_type;
    case CdcMuxDecodeError::unsupported_channel:
    case CdcMuxDecodeError::invalid_type_channel:
        return error_unsupported_channel;
    case CdcMuxDecodeError::none:
    case CdcMuxDecodeError::malformed_cobs:
    case CdcMuxDecodeError::frame_too_short:
    case CdcMuxDecodeError::bad_magic:
    case CdcMuxDecodeError::unsupported_flags:
        return 0U;
    }

    return 0U;
}

} // namespace

Status CdcMuxHostSession::initialize(CdcMuxWireWriteCallback wire_writer,
                                     CdcMuxChannelDataCallback data_callback,
                                     CdcMuxChannelStateCallback state_callback,
                                     void* callback_context) noexcept
{
    if (initialized_ || wire_writer == nullptr || data_callback == nullptr ||
            state_callback == nullptr)
    {
        return {StatusCode::invalid_argument,
                "invalid or repeated CDC mux host-session initialization"};
    }

    wire_writer_ = wire_writer;
    data_callback_ = data_callback;
    state_callback_ = state_callback;
    callback_context_ = callback_context;
    initialized_ = true;
    return Status::success();
}

Status CdcMuxHostSession::begin_handshake() noexcept
{
    if (!initialized_)
    {
        return {StatusCode::invalid_argument, "CDC mux host session is not initialized"};
    }

    clear_channels();
    decoder_.reset();
    established_ = false;
    handshake_pending_ = true;
    negotiated_payload_size_ = 0U;
    next_transmit_sequence_ = 1U;
    expected_receive_sequence_ = 1U;
    last_peer_error_ = 0U;

    constexpr std::array<std::byte, 1U> delimiter{std::byte{0U}};
    auto status = wire_writer_(callback_context_, delimiter);

    if (!status.is_ok())
    {
        handshake_pending_ = false;
        return status;
    }

    std::array<std::byte, 8U> payload{};
    payload[0] = static_cast<std::byte>(cdc_mux_protocol_version);
    payload[1] = static_cast<std::byte>(cdc_mux_protocol_version);
    write_u16(payload, 2U,
              static_cast<std::uint16_t>(cdc_mux_maximum_payload_size));
    write_u32(payload, 4U, cdc_mux_mandatory_capabilities);
    status = send_frame(CdcMuxFrameType::hello, CdcMuxChannel::link, 0U, payload);

    if (!status.is_ok())
    {
        handshake_pending_ = false;
    }

    return status;
}

Status CdcMuxHostSession::ingest_wire(std::span<const std::byte> bytes) noexcept
{
    return decoder_.ingest(bytes, &CdcMuxHostSession::receive_packet, this);
}

Status CdcMuxHostSession::open_channel(CdcMuxChannel channel) noexcept
{
    if (!established_ || !application_channel(channel))
    {
        return {StatusCode::invalid_argument,
                "CDC mux channel cannot be opened in the current session"};
    }

    if (channel_open(channel))
    {
        return Status::success();
    }

    const auto status = send_frame(CdcMuxFrameType::channel_open, channel,
                                   next_transmit_sequence_, {});

    if (status.is_ok())
    {
        ++next_transmit_sequence_;
        return update_channel(channel, true);
    }

    return status;
}

Status CdcMuxHostSession::close_channel(CdcMuxChannel channel) noexcept
{
    if (!established_ || !application_channel(channel))
    {
        return {StatusCode::invalid_argument,
                "CDC mux channel cannot be closed in the current session"};
    }

    if (!channel_open(channel))
    {
        return Status::success();
    }

    const auto status = send_frame(CdcMuxFrameType::channel_close, channel,
                                   next_transmit_sequence_, {});

    if (status.is_ok())
    {
        ++next_transmit_sequence_;
        return update_channel(channel, false);
    }

    return status;
}

Status CdcMuxHostSession::send_stream(CdcMuxChannel channel,
                                      std::span<const std::byte> bytes) noexcept
{
    if (!established_ || !channel_open(channel))
    {
        return {StatusCode::unavailable, "CDC mux host channel is closed"};
    }

    const std::size_t maximum = negotiated_payload_size_;
    std::size_t offset = 0U;

    while (offset < bytes.size())
    {
        const auto chunk_size = std::min(maximum, bytes.size() - offset);
        const auto status = send_frame(CdcMuxFrameType::stream_data, channel,
                                       next_transmit_sequence_,
                                       bytes.subspan(offset, chunk_size));

        if (!status.is_ok())
        {
            return status;
        }

        ++next_transmit_sequence_;
        offset += chunk_size;
    }

    return Status::success();
}

void CdcMuxHostSession::reset() noexcept
{
    clear_channels();
    decoder_.reset();
    established_ = false;
    handshake_pending_ = false;
    negotiated_payload_size_ = 0U;
    next_transmit_sequence_ = 1U;
    expected_receive_sequence_ = 1U;
}

bool CdcMuxHostSession::channel_open(CdcMuxChannel channel) const noexcept
{
    const auto index = channel_index(channel);
    return index < channels_.size() && channels_[index];
}

Status CdcMuxHostSession::receive_packet(void* context,
                                         const CdcMuxDecodedPacket& packet) noexcept
{
    return static_cast<CdcMuxHostSession*>(context)->handle_packet(packet);
}

Status CdcMuxHostSession::handle_packet(const CdcMuxDecodedPacket& packet) noexcept
{
    if (!packet.valid())
    {
        return Status::success();
    }

    const auto& frame = packet.frame;

    if (handshake_pending_ && frame.type == CdcMuxFrameType::hello_ack &&
            frame.sequence == 0U)
    {
        const auto selected_version = std::to_integer<std::uint8_t>(frame.payload[0]);
        const auto handshake_status = std::to_integer<std::uint8_t>(frame.payload[1]);
        const auto payload_size = read_u16(frame.payload, 2U);
        const auto capabilities = read_u32(frame.payload, 4U);

        if (selected_version != cdc_mux_protocol_version || handshake_status != 0U ||
                payload_size == 0U || payload_size > cdc_mux_maximum_payload_size ||
                (capabilities & cdc_mux_mandatory_capabilities) !=
                    cdc_mux_mandatory_capabilities)
        {
            reset();
            return {StatusCode::invalid_configuration,
                    "CDC mux device rejected or malformed the handshake"};
        }

        negotiated_payload_size_ = payload_size;
        handshake_pending_ = false;
        established_ = true;
        return Status::success();
    }

    if (!established_ || frame.sequence != expected_receive_sequence_)
    {
        reset();
        return {StatusCode::io_error, "CDC mux host receive sequence mismatch"};
    }

    ++expected_receive_sequence_;

    if (frame.type == CdcMuxFrameType::stream_data)
    {
        if (!channel_open(frame.channel))
        {
            return {StatusCode::io_error, "CDC mux device wrote a closed host channel"};
        }

        return data_callback_(callback_context_, frame.channel, frame.payload);
    }

    if (frame.type == CdcMuxFrameType::error)
    {
        last_peer_error_ = read_u16(frame.payload, 0U);
        return Status::success();
    }

    if (frame.type == CdcMuxFrameType::reset)
    {
        reset();
        return Status::success();
    }

    reset();
    return {StatusCode::io_error, "unexpected CDC mux device frame"};
}

Status CdcMuxHostSession::send_frame(CdcMuxFrameType type, CdcMuxChannel channel,
                                     std::uint32_t sequence,
                                     std::span<const std::byte> payload) noexcept
{
    std::array<std::byte, cdc_mux_maximum_wire_frame_size> wire{};
    const auto encoded = encode_cdc_mux_frame({type, channel, sequence, payload}, wire);

    if (!encoded)
    {
        return encoded.status();
    }

    return wire_writer_(callback_context_,
                        std::span<const std::byte>{wire.data(), encoded.value()});
}

Status CdcMuxHostSession::update_channel(CdcMuxChannel channel, bool open) noexcept
{
    const auto index = channel_index(channel);

    if (index >= channels_.size())
    {
        return {StatusCode::invalid_argument, "CDC mux channel index is invalid"};
    }

    channels_[index] = open;
    const auto status = state_callback_(callback_context_, channel, open);

    if (!status.is_ok())
    {
        channels_[index] = !open;
    }

    return status;
}

void CdcMuxHostSession::clear_channels() noexcept
{
    for (std::size_t index = 1U; index < channels_.size(); ++index)
    {
        if (channels_[index])
        {
            channels_[index] = false;
            static_cast<void>(state_callback_(callback_context_,
                static_cast<CdcMuxChannel>(index), false));
        }
    }
}

Status CdcMuxDeviceSession::initialize(CdcMuxWireWriteCallback wire_writer,
                                       CdcMuxChannelDataCallback data_callback,
                                       CdcMuxChannelStateCallback state_callback,
                                       CdcMuxLinkStateCallback link_state_callback,
                                       void* callback_context) noexcept
{
    if (initialized_ || wire_writer == nullptr || data_callback == nullptr ||
            state_callback == nullptr || link_state_callback == nullptr)
    {
        return {StatusCode::invalid_argument,
                "invalid or repeated CDC mux device-session initialization"};
    }

    wire_writer_ = wire_writer;
    data_callback_ = data_callback;
    state_callback_ = state_callback;
    link_state_callback_ = link_state_callback;
    callback_context_ = callback_context;
    initialized_ = true;
    return Status::success();
}

Status CdcMuxDeviceSession::ingest_wire(std::span<const std::byte> bytes) noexcept
{
    return decoder_.ingest(bytes, &CdcMuxDeviceSession::receive_packet, this);
}

Status CdcMuxDeviceSession::send_stream(CdcMuxChannel channel,
                                        std::span<const std::byte> bytes) noexcept
{
    if (!established_ || !channel_open(channel))
    {
        return {StatusCode::unavailable, "CDC mux device channel is closed"};
    }

    const std::size_t maximum = negotiated_payload_size_;
    std::size_t offset = 0U;

    while (offset < bytes.size())
    {
        const auto chunk_size = std::min(maximum, bytes.size() - offset);
        const auto status = send_frame(CdcMuxFrameType::stream_data, channel,
                                       next_transmit_sequence_,
                                       bytes.subspan(offset, chunk_size));

        if (!status.is_ok())
        {
            return status;
        }

        ++next_transmit_sequence_;
        offset += chunk_size;
    }

    return Status::success();
}

void CdcMuxDeviceSession::reset() noexcept
{
    abandon_session();
    decoder_.reset();
}

bool CdcMuxDeviceSession::channel_open(CdcMuxChannel channel) const noexcept
{
    const auto index = channel_index(channel);
    return index < channels_.size() && channels_[index];
}

Status CdcMuxDeviceSession::receive_packet(void* context,
                                           const CdcMuxDecodedPacket& packet) noexcept
{
    return static_cast<CdcMuxDeviceSession*>(context)->handle_packet(packet);
}

Status CdcMuxDeviceSession::handle_packet(const CdcMuxDecodedPacket& packet) noexcept
{
    if (!packet.valid())
    {
        const auto error_code = decode_error_code(packet.error);

        if (established_ && error_code != 0U)
        {
            return send_error(error_code);
        }

        return Status::success();
    }

    const auto& frame = packet.frame;

    if (frame.type == CdcMuxFrameType::hello && frame.sequence == 0U)
    {
        return accept_hello(frame);
    }

    if (!established_)
    {
        return Status::success();
    }

    if (frame.sequence != expected_receive_sequence_)
    {
        const auto status = send_error(error_sequence);
        abandon_session();
        return status;
    }

    ++expected_receive_sequence_;

    if (frame.type == CdcMuxFrameType::channel_open)
    {
        if (!application_channel(frame.channel))
        {
            return send_error(error_service_unavailable);
        }

        return update_channel(frame.channel, true);
    }

    if (frame.type == CdcMuxFrameType::channel_close)
    {
        if (!application_channel(frame.channel))
        {
            return send_error(error_unsupported_channel);
        }

        return update_channel(frame.channel, false);
    }

    if (frame.type == CdcMuxFrameType::stream_data)
    {
        if (!channel_open(frame.channel))
        {
            return send_error(error_channel_closed);
        }

        return data_callback_(callback_context_, frame.channel, frame.payload);
    }

    if (frame.type == CdcMuxFrameType::reset)
    {
        abandon_session();
        return Status::success();
    }

    if (frame.type == CdcMuxFrameType::error)
    {
        return Status::success();
    }

    return send_error(error_unsupported_type);
}

Status CdcMuxDeviceSession::accept_hello(const CdcMuxFrameView& frame) noexcept
{
    abandon_session();
    const auto minimum_version = std::to_integer<std::uint8_t>(frame.payload[0]);
    const auto maximum_version = std::to_integer<std::uint8_t>(frame.payload[1]);
    const auto host_payload_size = read_u16(frame.payload, 2U);
    const auto host_capabilities = read_u32(frame.payload, 4U);
    const bool accepted = minimum_version <= cdc_mux_protocol_version &&
                          maximum_version >= cdc_mux_protocol_version &&
                          host_payload_size != 0U &&
                          (host_capabilities & cdc_mux_mandatory_capabilities) ==
                              cdc_mux_mandatory_capabilities;

    negotiated_payload_size_ = static_cast<std::uint16_t>(std::min<std::size_t>(
        host_payload_size, cdc_mux_maximum_payload_size));

    std::array<std::byte, 8U> payload{};
    payload[0] = static_cast<std::byte>(cdc_mux_protocol_version);
    payload[1] = static_cast<std::byte>(accepted ? 0U : 1U);
    write_u16(payload, 2U, accepted ? negotiated_payload_size_ : 0U);
    write_u32(payload, 4U, accepted ? cdc_mux_mandatory_capabilities : 0U);
    const auto status = send_frame(CdcMuxFrameType::hello_ack,
                                   CdcMuxChannel::link, 0U, payload);

    if (status.is_ok() && accepted)
    {
        established_ = true;
        next_transmit_sequence_ = 1U;
        expected_receive_sequence_ = 1U;
        link_state_callback_(callback_context_, true);
    }

    return status;
}

Status CdcMuxDeviceSession::send_frame(CdcMuxFrameType type,
                                       CdcMuxChannel channel,
                                       std::uint32_t sequence,
                                       std::span<const std::byte> payload) noexcept
{
    std::array<std::byte, cdc_mux_maximum_wire_frame_size> wire{};
    const auto encoded = encode_cdc_mux_frame({type, channel, sequence, payload}, wire);

    if (!encoded)
    {
        return encoded.status();
    }

    return wire_writer_(callback_context_,
                        std::span<const std::byte>{wire.data(), encoded.value()});
}

Status CdcMuxDeviceSession::send_error(std::uint16_t error_code) noexcept
{
    std::array<std::byte, 2U> payload{};
    write_u16(payload, 0U, error_code);
    const auto status = send_frame(CdcMuxFrameType::error, CdcMuxChannel::link,
                                   next_transmit_sequence_, payload);

    if (status.is_ok())
    {
        ++next_transmit_sequence_;
    }

    return status;
}

Status CdcMuxDeviceSession::update_channel(CdcMuxChannel channel, bool open) noexcept
{
    const auto index = channel_index(channel);

    if (index >= channels_.size())
    {
        return send_error(error_unsupported_channel);
    }

    if (channels_[index] == open)
    {
        return Status::success();
    }

    channels_[index] = open;
    const auto status = state_callback_(callback_context_, channel, open);

    if (!status.is_ok())
    {
        channels_[index] = !open;
    }

    return status;
}

void CdcMuxDeviceSession::clear_channels() noexcept
{
    for (std::size_t index = 1U; index < channels_.size(); ++index)
    {
        if (channels_[index])
        {
            channels_[index] = false;
            static_cast<void>(state_callback_(callback_context_,
                static_cast<CdcMuxChannel>(index), false));
        }
    }
}

void CdcMuxDeviceSession::abandon_session() noexcept
{
    const bool was_established = established_;
    clear_channels();
    established_ = false;
    negotiated_payload_size_ = 0U;
    next_transmit_sequence_ = 1U;
    expected_receive_sequence_ = 1U;

    if (was_established)
    {
        link_state_callback_(callback_context_, false);
    }
}

} // namespace m110
