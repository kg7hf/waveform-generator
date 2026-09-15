// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Paul R. Decker

#pragma once

#include "common/status.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace m110
{

enum class CdcMuxFrameType : std::uint8_t
{
    hello = 0x01U,
    hello_ack = 0x02U,
    channel_open = 0x10U,
    channel_close = 0x11U,
    stream_data = 0x12U,
    reset = 0x13U,
    error = 0x14U,
};

enum class CdcMuxChannel : std::uint8_t
{
    link = 0x00U,
    control_status = 0x01U,
    binary_data = 0x02U,
    provisioning = 0x03U,
    diagnostic = 0x04U,
};

enum class CdcMuxDecodeError : std::uint8_t
{
    none = 0U,
    packet_too_large,
    malformed_cobs,
    frame_too_short,
    bad_magic,
    unsupported_version,
    unsupported_flags,
    length_mismatch,
    bad_crc,
    unsupported_type,
    unsupported_channel,
    invalid_type_channel,
};

struct CdcMuxFrameView
{
    CdcMuxFrameType type{};
    CdcMuxChannel channel{};
    std::uint32_t sequence{};
    std::span<const std::byte> payload{};
};

struct CdcMuxDecodedPacket
{
    CdcMuxDecodeError error{CdcMuxDecodeError::none};
    CdcMuxFrameView frame{};

    [[nodiscard]] bool valid() const noexcept
    {
        return error == CdcMuxDecodeError::none;
    }
};

using CdcMuxPacketCallback = Status (*)(void* context,
                                        const CdcMuxDecodedPacket& packet) noexcept;

struct CdcMuxDecoderStatistics
{
    std::uint32_t valid_frames{};
    std::uint32_t empty_packets{};
    std::uint32_t rejected_packets{};
    std::uint32_t oversized_packets{};
};

inline constexpr std::uint8_t cdc_mux_protocol_version = 1U;
inline constexpr std::size_t cdc_mux_header_size = 12U;
inline constexpr std::size_t cdc_mux_crc_size = 4U;

#ifndef M110_CDC_MUX_MAXIMUM_PAYLOAD_SIZE
#define M110_CDC_MUX_MAXIMUM_PAYLOAD_SIZE 256U
#endif

inline constexpr std::size_t cdc_mux_maximum_payload_size =
    M110_CDC_MUX_MAXIMUM_PAYLOAD_SIZE;
inline constexpr std::size_t cdc_mux_maximum_decoded_frame_size =
    cdc_mux_header_size + cdc_mux_maximum_payload_size + cdc_mux_crc_size;
inline constexpr std::size_t cdc_mux_maximum_cobs_packet_size =
    cdc_mux_maximum_decoded_frame_size +
    (cdc_mux_maximum_decoded_frame_size / 254U) + 1U;
inline constexpr std::size_t cdc_mux_maximum_wire_frame_size =
    cdc_mux_maximum_cobs_packet_size + 1U;

static_assert(cdc_mux_maximum_payload_size >= 256U);
static_assert(cdc_mux_maximum_payload_size <= UINT16_MAX);

[[nodiscard]] std::uint32_t cdc_mux_crc32c(
    std::span<const std::byte> bytes) noexcept;

// Encode one complete DD-008 frame, including its trailing zero delimiter.
// The returned size is the number of bytes written to output.
[[nodiscard]] Result<std::size_t> encode_cdc_mux_frame(
    const CdcMuxFrameView& frame,
    std::span<std::byte> output) noexcept;

// Streaming COBS decoder. Read/USB-transfer boundaries are deliberately
// ignored; one callback is issued for every nonempty delimiter-terminated
// packet, including rejected packets so the session layer can count/report it.
class CdcMuxStreamDecoder
{
public:
    [[nodiscard]] Status ingest(std::span<const std::byte> bytes,
                                CdcMuxPacketCallback callback,
                                void* callback_context) noexcept;

    void reset() noexcept;

    [[nodiscard]] const CdcMuxDecoderStatistics& statistics() const noexcept
    {
        return statistics_;
    }

private:
    [[nodiscard]] Status finish_packet(CdcMuxPacketCallback callback,
                                       void* callback_context) noexcept;

    std::array<std::byte, cdc_mux_maximum_cobs_packet_size> encoded_{};
    std::array<std::byte, cdc_mux_maximum_decoded_frame_size> decoded_{};
    std::size_t encoded_size_{};
    CdcMuxDecoderStatistics statistics_{};
    bool discarding_oversized_packet_{};
};

} // namespace m110
