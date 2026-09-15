// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Paul R. Decker

#pragma once

#include "common/dd008/cdc_mux_protocol.hpp"

#include <array>
#include <cstdint>
#include <span>

namespace m110
{

using CdcMuxWireWriteCallback = Status (*)(void* context,
                                std::span<const std::byte> bytes) noexcept;
using CdcMuxChannelDataCallback = Status (*)(void* context,
                                  CdcMuxChannel channel,
                                  std::span<const std::byte> bytes) noexcept;
using CdcMuxChannelStateCallback = Status (*)(void* context,
                                   CdcMuxChannel channel,
                                   bool open) noexcept;
using CdcMuxLinkStateCallback = void (*)(void* context, bool established) noexcept;

inline constexpr std::uint32_t cdc_mux_control_status_capability = 1U << 0U;
inline constexpr std::uint32_t cdc_mux_binary_data_capability = 1U << 1U;
inline constexpr std::uint32_t cdc_mux_mandatory_capabilities =
    cdc_mux_control_status_capability | cdc_mux_binary_data_capability;

class CdcMuxHostSession
{
public:
    [[nodiscard]] Status initialize(CdcMuxWireWriteCallback wire_writer,
                                    CdcMuxChannelDataCallback data_callback,
                                    CdcMuxChannelStateCallback state_callback,
                                    void* callback_context) noexcept;
    [[nodiscard]] Status begin_handshake() noexcept;
    [[nodiscard]] Status ingest_wire(std::span<const std::byte> bytes) noexcept;
    [[nodiscard]] Status open_channel(CdcMuxChannel channel) noexcept;
    [[nodiscard]] Status close_channel(CdcMuxChannel channel) noexcept;
    [[nodiscard]] Status send_stream(CdcMuxChannel channel,
                                     std::span<const std::byte> bytes) noexcept;
    void reset() noexcept;

    [[nodiscard]] bool established() const noexcept
    {
        return established_;
    }
    [[nodiscard]] bool channel_open(CdcMuxChannel channel) const noexcept;
    [[nodiscard]] std::uint16_t negotiated_payload_size() const noexcept
    {
        return negotiated_payload_size_;
    }
    [[nodiscard]] std::uint16_t last_peer_error() const noexcept
    {
        return last_peer_error_;
    }

private:
    static Status receive_packet(void* context,
                                 const CdcMuxDecodedPacket& packet) noexcept;
    [[nodiscard]] Status handle_packet(const CdcMuxDecodedPacket& packet) noexcept;
    [[nodiscard]] Status send_frame(CdcMuxFrameType type, CdcMuxChannel channel,
                                    std::uint32_t sequence,
                                    std::span<const std::byte> payload) noexcept;
    [[nodiscard]] Status update_channel(CdcMuxChannel channel, bool open) noexcept;
    void clear_channels() noexcept;

    CdcMuxStreamDecoder decoder_{};
    CdcMuxWireWriteCallback wire_writer_{};
    CdcMuxChannelDataCallback data_callback_{};
    CdcMuxChannelStateCallback state_callback_{};
    void* callback_context_{};
    std::array<bool, 5U> channels_{};
    std::uint32_t next_transmit_sequence_{1U};
    std::uint32_t expected_receive_sequence_{1U};
    std::uint16_t negotiated_payload_size_{};
    std::uint16_t last_peer_error_{};
    bool initialized_{};
    bool handshake_pending_{};
    bool established_{};
};

class CdcMuxDeviceSession
{
public:
    [[nodiscard]] Status initialize(CdcMuxWireWriteCallback wire_writer,
                                    CdcMuxChannelDataCallback data_callback,
                                    CdcMuxChannelStateCallback state_callback,
                                    CdcMuxLinkStateCallback link_state_callback,
                                    void* callback_context) noexcept;
    [[nodiscard]] Status ingest_wire(std::span<const std::byte> bytes) noexcept;
    [[nodiscard]] Status send_stream(CdcMuxChannel channel,
                                     std::span<const std::byte> bytes) noexcept;
    void reset() noexcept;

    [[nodiscard]] bool established() const noexcept
    {
        return established_;
    }
    [[nodiscard]] bool channel_open(CdcMuxChannel channel) const noexcept;

private:
    static Status receive_packet(void* context,
                                 const CdcMuxDecodedPacket& packet) noexcept;
    [[nodiscard]] Status handle_packet(const CdcMuxDecodedPacket& packet) noexcept;
    [[nodiscard]] Status accept_hello(const CdcMuxFrameView& frame) noexcept;
    [[nodiscard]] Status send_frame(CdcMuxFrameType type, CdcMuxChannel channel,
                                    std::uint32_t sequence,
                                    std::span<const std::byte> payload) noexcept;
    [[nodiscard]] Status send_error(std::uint16_t error_code) noexcept;
    [[nodiscard]] Status update_channel(CdcMuxChannel channel, bool open) noexcept;
    void clear_channels() noexcept;
    void abandon_session() noexcept;

    CdcMuxStreamDecoder decoder_{};
    CdcMuxWireWriteCallback wire_writer_{};
    CdcMuxChannelDataCallback data_callback_{};
    CdcMuxChannelStateCallback state_callback_{};
    CdcMuxLinkStateCallback link_state_callback_{};
    void* callback_context_{};
    std::array<bool, 5U> channels_{};
    std::uint32_t next_transmit_sequence_{1U};
    std::uint32_t expected_receive_sequence_{1U};
    std::uint16_t negotiated_payload_size_{};
    bool initialized_{};
    bool established_{};
};

} // namespace m110
