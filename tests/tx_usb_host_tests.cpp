// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Paul R. Decker

// Actual DD008 USB adapter with deterministic endpoint and storage-owner stubs.
// This checks protocol/mailbox behavior, not USB timing or filesystem durability.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

#include "rt1170/tx_usb.cpp"

namespace
{
int failures{};
TestOcotp fuses{{{0}, {0x12345678}, {0x9ABCDEF0}}};
std::deque<std::byte> inbound;
std::vector<std::byte> outbound;
std::string responses;
waveform_generator::TxArtifactSnapshot artifact;
waveform_generator::tx_protocol::Request owner_request;
waveform_generator::tx_protocol::Reply owner_reply;
bool owner_pending{}, reply_pending{};
std::size_t write_budget{100000};
std::vector<waveform_generator::tx_protocol::Request> accepted_requests;

void check(bool value, const char* message)
{
    if (!value)
    {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

m110::Status host_write(void*, std::span<const std::byte> bytes) noexcept
{
    inbound.insert(inbound.end(), bytes.begin(), bytes.end());
    return m110::Status::success();
}
m110::Status host_read(void*, m110::CdcMuxChannel, std::span<const std::byte> bytes) noexcept
{
    responses.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    return m110::Status::success();
}
m110::Status host_state(void*, m110::CdcMuxChannel, bool) noexcept
{
    return m110::Status::success();
}

void pump(m110::CdcMuxHostSession& host)
{
    for (unsigned int count = 0; count < 10; ++count)
    {
        waveform_generator::tx_usb_service();

        while (!inbound.empty() && !waveform_generator::tx_usb_blocked())
        {
            const auto byte = inbound.front();
            inbound.pop_front();
            waveform_generator::tx_usb_feed(std::to_integer<std::uint8_t>(byte));
        }

        waveform_generator::tx_usb_service();

        if (!outbound.empty())
        {
            check(host.ingest_wire(outbound).is_ok(), "device response decodes with ordered sequence");
            outbound.clear();
        }
    }
}

void owner_step()
{
    using namespace waveform_generator::tx_protocol;
    check(owner_pending, "owner has request");

    if (!owner_pending)
    {
        return;
    }

    switch (owner_request.kind)
    {
        case Kind::mode:
            artifact.rate = owner_request.rate;
            artifact.interleave = owner_request.interleave;
            break;

        case Kind::data:
            artifact.payload_bytes += owner_request.size;
            artifact.state = 1;
            break;

        case Kind::generate_file:
            artifact.state = 2;
            std::strcpy(artifact.filename, owner_request.name);
            break;

        default:
            break;
    }

    owner_reply = {true, "none", artifact.payload_bytes, artifact.frames_written, artifact.state == 2, false};
    reply_pending = true;
    owner_pending = false;
}

void command(m110::CdcMuxHostSession& host, const char* text)
{
    check(host.send_stream(m110::CdcMuxChannel::control_status,
    {reinterpret_cast<const std::byte*>(text), std::strlen(text)}).is_ok(), "host control send");
    pump(host);
}

void connect(m110::CdcMuxHostSession& host)
{
    // Exact imported Python host preface includes an empty packet before HELLO.
    inbound.push_back(std::byte{0});
    check(host.begin_handshake().is_ok(), "begin HELLO");
    pump(host);
    check(host.established(), "adapter HELLO established");
    check(host.open_channel(m110::CdcMuxChannel::control_status).is_ok(), "open control");
    check(host.open_channel(m110::CdcMuxChannel::binary_data).is_ok(), "open data");
    pump(host);
}
}

TestOcotp* OCOTP = &fuses;

extern "C" std::uint32_t m110_tinyusb_cdc_write(const void* data, std::uint32_t size)
{
    const auto count = std::min<std::size_t>(size, write_budget);
    const auto* bytes = static_cast<const std::byte*>(data);
    outbound.insert(outbound.end(), bytes, bytes + count);
    write_budget -= count;
    return static_cast<std::uint32_t>(count);
}
extern "C" void m110_tinyusb_cdc_flush() {}

namespace waveform_generator
{
bool submit_tx_request(const tx_protocol::Request& request) noexcept
{
    if (owner_pending || reply_pending)
    {
        return false;
    }

    owner_request = request;
    accepted_requests.push_back(request);
    owner_pending = true;
    return true;
}
bool take_tx_reply(tx_protocol::Reply& reply) noexcept
{
    if (!reply_pending)
    {
        return false;
    }

    reply = owner_reply;
    reply_pending = false;
    return true;
}
TxArtifactSnapshot tx_artifact_snapshot() noexcept
{
    return artifact;
}
void disconnect_tx_artifact() noexcept
{
    owner_pending = reply_pending = false;
}
}

int main()
{
    using namespace waveform_generator;
    m110::CdcMuxHostSession host;
    check(host.initialize(host_write, host_read, host_state, nullptr).is_ok(), "host initialize");
    connect(host);
    command(host, "CMD:MODEM INFO:?\n");
    check(responses.find("SERIAL=123456789ABCDEF0") != std::string::npos &&
          responses.find("KIND=waveform-generator") != std::string::npos, "engineering identity");
    responses.clear();
    std::array<std::byte, 256> data;

    for (std::size_t index = 0; index < data.size(); ++index)
    {
        data[index] = static_cast<std::byte>(index);
    }

    check(host.send_stream(m110::CdcMuxChannel::binary_data, data).is_ok(), "send data");
    pump(host);
    check(owner_pending && tx_usb_blocked() && responses.empty(), "no data acknowledgment before owner accepts");
    check(owner_request.size == 256 && std::memcmp(owner_request.data, data.data(), 256) == 0, "binary mailbox copy");
    owner_step();
    write_budget = 7;
    pump(host);
    check(tx_usb_blocked() && responses.empty(), "partial USB write retains response and blocks input");
    write_budget = 100000;
    pump(host);
    check(responses == "DATA:256\n" && !tx_usb_blocked(), "data acknowledgment after owner acceptance");
    responses.clear();
    check(host.send_stream(m110::CdcMuxChannel::binary_data, data).is_ok(), "first queued data frame");
    check(host.send_stream(m110::CdcMuxChannel::binary_data, data).is_ok(), "second queued data frame");
    pump(host);
    check(owner_pending && !inbound.empty(), "second wire frame remains unread while first owner request waits");
    owner_step();
    pump(host);
    check(owner_pending && owner_request.kind == tx_protocol::Kind::data && inbound.empty(),
          "next data frame resumes after the preceding owner acknowledgment");
    owner_step();
    pump(host);
    check(responses == "DATA:512\nDATA:768\n", "queued frames have distinct ordered owner acknowledgments");
    responses.clear();
    command(host, "CMD:DATA RATE:1200SCMD:TX FILE:INPUT.BIN:OUT.WAV\n");
    check(owner_pending && owner_request.kind == tx_protocol::Kind::mode, "first concatenated command waits for owner");
    owner_step();
    pump(host);
    check(owner_pending && owner_request.kind == tx_protocol::Kind::generate_file &&
          owner_request.rate == 1200 && owner_request.interleave == 0 &&
          std::strcmp(owner_request.input_name, "INPUT.BIN") == 0, "next file command uses acknowledged owner mode");
    owner_step();
    pump(host);
    check(responses == "OK\nSTATUS:TX:GENERATING\n", "generation acknowledgment is distinct from playback");
    responses.clear();
    tx_usb_disconnect();
    host.reset();
    connect(host);
    command(host, "CMD:DATA RATE:?\n");
    check(responses == "1200S\n", "reconnect mode readback uses owner state");
    responses.clear();
    artifact.state = 3;
    artifact.frames_written = artifact.total_frames = 1000;
    std::memset(artifact.wav_sha256, 'a', 64);
    command(host, "CMD:STATUS:?\n");
    check(responses.find("STATE=3;FILE=OUT.WAV;PAYLOAD=768;FRAMES=1000;TOTAL=1000") != std::string::npos,
          "completed artifact status");
    tx_usb_disconnect();
    return failures == 0 ? 0 : 1;
}
