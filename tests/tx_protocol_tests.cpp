// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Paul R. Decker

#include "common/tx_protocol.hpp"
#include "common/dd008/cdc_mux_session.hpp"

#include <array>
#include <cstdio>
#include <cstring>
#include <string_view>
#include <vector>

namespace
{
int failures{};
void check(bool value, const char* message)
{
    if (!value)
    {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}
using namespace waveform_generator::tx_protocol;

void commands()
{
    CommandParser parser;
    std::vector<Request> requests;
    const auto feed = [&](std::string_view text)
    {
        for (const auto byte : text)
        {
            parser.feed(static_cast<std::uint8_t>(byte));

            if (parser.ready())
            {
                check(parser.valid(), "valid command fragmented/concatenated");
                requests.push_back(parser.request());
                parser.consume();
            }
        }
    };
    feed("CMD:MODE");
    check(requests.empty(), "fragment held");
    feed("M INFO:?CMD:DATA RATE:600LCMD:SENDBUFFER\r\nCMD:RESET MDM");
    check(requests.size() == 4U, "four production commands");
    check(requests[0].kind == Kind::info && requests[1].kind == Kind::mode &&
          requests[1].rate == 600U && requests[1].interleave == 1U &&
          requests[2].kind == Kind::send && requests[3].kind == Kind::reset, "production semantics");
    feed("CMD:WAV FILE:FRESH123.WAV\nCMD:TX FILE:INPUT.BIN:OUT.WAV\nCMD:STATUS:?\n");
    check(requests.size() == 7U && requests[4].kind == Kind::select &&
          std::strcmp(requests[4].name, "FRESH123.WAV") == 0 &&
          requests[5].kind == Kind::generate_file &&
          std::strcmp(requests[5].input_name, "INPUT.BIN") == 0 &&
          std::strcmp(requests[5].name, "OUT.WAV") == 0, "artifact filenames");

    for (const auto* token :
{"75S", "75L", "150S", "150L", "300S", "300L", "600S",
    "600L", "1200S", "1200L", "2400S", "2400L", "4800U"
})
    {
        feed("CMD:DATA RATE:");
        feed(token);
        const auto& request = requests.back();
        check(std::strcmp(mode_token(request.rate, request.interleave), token) == 0, "all thirteen modes");
    }

    for (const auto* text :
{"CMD:WAV FILE:../A.WAV\n", "CMD:WAV FILE:a.WAV\n",
    "CMD:TX FILE:A.WAV:B.WAV\n", "CMD:DATA RATE:4800L\n",
    "CMD:TX FILE:A.BIN:B.WAV:C.WAV\n"
})
    {
        parser.reset();

        for (const auto* at = text; *at; ++at)
        {
            parser.feed(static_cast<std::uint8_t>(*at));
        }

        check(parser.ready() && !parser.valid(), "invalid command rejected");
    }
    parser.reset();

    for (int count = 0; count < 2048; ++count)
    {
        parser.feed('X');
    }

    parser.feed('\n');
    check(parser.ready() && !parser.valid(), "overflow bounded");
    parser.consume();
    feed("CMD:MODEM INFO:?");
    check(requests.back().kind == Kind::info, "overflow recovery at line boundary");
}

struct Endpoint
{
    std::vector<std::byte> wire;
    std::vector<std::byte> data;
    static m110::Status write(void* context, std::span<const std::byte> bytes) noexcept
    {
        auto& self = *static_cast<Endpoint*>(context);
        self.wire.insert(self.wire.end(), bytes.begin(), bytes.end());
        return m110::Status::success();
    }
    static m110::Status receive(void* context, m110::CdcMuxChannel, std::span<const std::byte> bytes) noexcept
    {
        auto& self = *static_cast<Endpoint*>(context);
        self.data.insert(self.data.end(), bytes.begin(), bytes.end());
        return m110::Status::success();
    }
    static m110::Status state(void*, m110::CdcMuxChannel, bool) noexcept
    {
        return m110::Status::success();
    }
    static void link(void*, bool) noexcept {}
};

void transport()
{
    Endpoint host_io, device_io;
    m110::CdcMuxHostSession host;
    m110::CdcMuxDeviceSession device;
    check(host.initialize(Endpoint::write, Endpoint::receive, Endpoint::state, &host_io).is_ok(), "host init");
    check(device.initialize(Endpoint::write, Endpoint::receive, Endpoint::state, Endpoint::link, &device_io).is_ok(), "device init");
    const auto pump_host = [&]
    {
        for (const auto value : host_io.wire)
        {
            check(device.ingest_wire({&value, 1U}).is_ok(), "fragmented host frame");
        }

        host_io.wire.clear();
    };
    const auto pump_device = [&]
    {
        for (const auto value : device_io.wire)
        {
            check(host.ingest_wire({&value, 1U}).is_ok(), "fragmented device frame");
        }

        device_io.wire.clear();
    };
    check(host.begin_handshake().is_ok(), "HELLO");
    pump_host();
    pump_device();
    check(host.established() && device.established() && host.negotiated_payload_size() == 256U, "256-byte HELLO negotiation");
    check(host.open_channel(m110::CdcMuxChannel::control_status).is_ok(), "control open");
    check(host.open_channel(m110::CdcMuxChannel::binary_data).is_ok(), "data open");
    pump_host();
    std::array<std::byte, 1024> payload;

    for (std::size_t index = 0; index < payload.size(); ++index)
    {
        payload[index] = static_cast<std::byte>(index & 255U);
    }

    check(host.send_stream(m110::CdcMuxChannel::binary_data, payload).is_ok(), "binary data send");
    pump_host();
    check(device_io.data == std::vector<std::byte>(payload.begin(), payload.end()), "all binary bytes retained over four frames");
    check(device.send_stream(m110::CdcMuxChannel::control_status, payload).is_ok(), "device response fragmentation");
    pump_device();
    check(host_io.data == std::vector<std::byte>(payload.begin(), payload.end()), "response sequence and content");
    check(host.close_channel(m110::CdcMuxChannel::binary_data).is_ok(), "close channel");
    pump_host();
    check(!device.channel_open(m110::CdcMuxChannel::binary_data), "channel close received");
    device.reset();
    check(!device.established(), "disconnect clears negotiated state");
    check(m110::cdc_mux_maximum_wire_frame_size == 275U, "wire bound");
}
}

int main()
{
    commands();
    transport();
    return failures == 0 ? 0 : 1;
}
