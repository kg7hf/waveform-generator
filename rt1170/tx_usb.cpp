#include "rt1170/tx_usb.hpp"
#include "rt1170/tx_artifact.hpp"
#include "common/dd008/cdc_mux_session.hpp"
#include "usb/usb_tinyusb_port.h"
#include "fsl_common.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <span>

namespace waveform_generator
{
namespace
{
using namespace tx_protocol;
m110::CdcMuxDeviceSession session;
CommandParser parser;
std::array<std::byte, 2048> wire;
std::size_t wire_size{}, wire_sent{};
std::array<std::uint8_t, maximum_payload> control;
std::size_t control_size{}, control_offset{};
Request pending;
bool initialized{}, active{}, request_pending{}, request_submitted{}, failed{}, had_link{};
std::uint32_t last_job_state{};

m110::Status write_wire(void*, std::span<const std::byte> bytes) noexcept
{
    if (bytes.size() > wire.size() - wire_size)
    {
        failed = true;
        return {m110::StatusCode::buffer_too_small, "DD008 output full"};
    }
    std::memcpy(wire.data() + wire_size, bytes.data(), bytes.size());
    wire_size += bytes.size();
    return m110::Status::success();
}

void respond(const char* text) noexcept
{
    if (!session.established() || !session.channel_open(m110::CdcMuxChannel::control_status))
    {
        return;
    }
    const auto result = session.send_stream(m110::CdcMuxChannel::control_status,
        {reinterpret_cast<const std::byte*>(text), std::strlen(text)});
    if (!result.is_ok())
    {
        failed = true;
    }
}

void respond_snapshot() noexcept
{
    const auto snapshot = tx_artifact_snapshot();
    char text[384]{};
    std::snprintf(text, sizeof(text),
        "STATUS:WAVEFORM FILE:STATE=%lu;FILE=%s;PAYLOAD=%lu;FRAMES=%llu;TOTAL=%llu;SHA256=%s;ERROR=%s\n",
        static_cast<unsigned long>(snapshot.state), snapshot.filename,
        static_cast<unsigned long>(snapshot.payload_bytes),
        static_cast<unsigned long long>(snapshot.frames_written),
        static_cast<unsigned long long>(snapshot.total_frames),
        snapshot.wav_sha256, snapshot.error);
    respond(text);
}

void respond_info() noexcept
{
    char text[320]{};
    std::snprintf(text, sizeof(text),
        "MODEM INFO:VERSION=0.2.0;BOARD=MIMXRT1170-EVK;KIND=waveform-generator;SERIAL=%08lX%08lX;PROTOCOL=DD008/1;MAX_UPLOAD=1048576;SOURCE_MANIFEST=%s\n",
        static_cast<unsigned long>(OCOTP->FUSEN[1].FUSE),
        static_cast<unsigned long>(OCOTP->FUSEN[2].FUSE), WFG_SOURCE_MANIFEST_SHA256);
    respond(text);
}

m110::Status receive_data(void*, m110::CdcMuxChannel channel,
                          std::span<const std::byte> bytes) noexcept
{
    if (request_pending || control_size != 0U || bytes.size() > maximum_payload)
    {
        failed = true;
        return {m110::StatusCode::busy, "DD008 input mailbox full"};
    }
    if (channel == m110::CdcMuxChannel::binary_data)
    {
        pending = {};
        pending.kind = Kind::data;
        pending.size = static_cast<std::uint16_t>(bytes.size());
        std::memcpy(pending.data, bytes.data(), bytes.size());
        request_pending = true;
    }
    else if (channel == m110::CdcMuxChannel::control_status)
    {
        std::memcpy(control.data(), bytes.data(), bytes.size());
        control_size = bytes.size();
        control_offset = 0U;
    }
    return m110::Status::success();
}

m110::Status channel_state(void*, m110::CdcMuxChannel channel, bool open) noexcept
{
    if (channel == m110::CdcMuxChannel::control_status && !open)
    {
        parser.reset();
        control_size = control_offset = 0U;
    }
    return m110::Status::success();
}

void link_state(void*, bool established) noexcept
{
    if (had_link && !established)
    {
        disconnect_tx_artifact();
        parser.reset();
        control_size = control_offset = 0U;
    }
    had_link = established;
}

void pump_commands() noexcept
{
    while (control_offset < control_size && !request_pending && wire_size == 0U)
    {
        parser.feed(control[control_offset++]);
        if (!parser.ready())
        {
            continue;
        }
        if (!parser.valid())
        {
            respond("ERROR:INVALID COMMAND\n");
        }
        else if (parser.mode_query())
        {
            char text[16]{};
            const auto snapshot = tx_artifact_snapshot();
            const auto* token = mode_token(snapshot.rate, snapshot.interleave);
            std::snprintf(text, sizeof(text), "%s\n", token ? token : "ERROR:BAD_MODE");
            respond(text);
        }
        else if (parser.request().kind == Kind::info)
        {
            respond_info();
        }
        else if (parser.request().kind == Kind::query)
        {
            respond_snapshot();
        }
        else
        {
            pending = parser.request();
            if (pending.kind == Kind::generate_file)
            {
                const auto snapshot = tx_artifact_snapshot();
                pending.rate = snapshot.rate;
                pending.interleave = snapshot.interleave;
            }
            request_pending = true;
        }
        parser.consume();
    }
    if (control_offset == control_size)
    {
        control_size = control_offset = 0U;
    }
}

void finish_request(const Reply& reply) noexcept
{
    if (!reply.ok)
    {
        char text[160]{};
        std::snprintf(text, sizeof(text), "ERROR:%s\n", reply.error);
        respond(text);
    }
    else if (pending.kind == Kind::data)
    {
        char text[48]{};
        std::snprintf(text, sizeof(text), "DATA:%lu\n", static_cast<unsigned long>(reply.payload_bytes));
        respond(text);
    }
    else if (pending.kind == Kind::send || pending.kind == Kind::generate_file)
    {
        respond("STATUS:TX:GENERATING\n");
        last_job_state = 2U;
    }
    else
    {
        respond("OK\n");
    }
    request_pending = request_submitted = false;
}
}

bool tx_usb_active() noexcept { return active; }

bool tx_usb_blocked() noexcept
{
    return failed || wire_size != 0U || request_pending || control_size != 0U;
}

void tx_usb_feed(std::uint8_t byte) noexcept
{
    if (tx_usb_blocked())
    {
        return;
    }
    if (!initialized)
    {
        initialized = session.initialize(write_wire, receive_data, channel_state, link_state, nullptr).is_ok();
        if (!initialized)
        {
            failed = true;
            return;
        }
    }
    active = true;
    const std::byte value{byte};
    if (!session.ingest_wire({&value, 1U}).is_ok())
    {
        failed = true;
    }
}

void tx_usb_service() noexcept
{
    if (!active)
    {
        return;
    }
    while (wire_sent < wire_size)
    {
        const auto count = m110_tinyusb_cdc_write(wire.data() + wire_sent,
            static_cast<std::uint32_t>(wire_size - wire_sent));
        if (count == 0U)
        {
            return;
        }
        wire_sent += count;
    }
    m110_tinyusb_cdc_flush();
    wire_size = wire_sent = 0U;
    if (failed)
    {
        return;
    }
    if (request_pending)
    {
        if (!request_submitted)
        {
            request_submitted = submit_tx_request(pending);
        }
        if (request_submitted)
        {
            Reply reply;
            if (take_tx_reply(reply))
            {
                finish_request(reply);
            }
        }
    }
    if (!request_pending && wire_size == 0U)
    {
        pump_commands();
    }
    if (!request_pending && control_size == 0U && wire_size == 0U)
    {
        const auto state = tx_artifact_snapshot().state;
        if (last_job_state == 2U && state != 2U)
        {
            respond_snapshot();
        }
        last_job_state = state;
    }
}

void tx_usb_disconnect() noexcept
{
    disconnect_tx_artifact();
    session.reset();
    parser.reset();
    wire_size = wire_sent = control_size = control_offset = 0U;
    request_pending = request_submitted = failed = active = had_link = false;
    last_job_state = 0U;
}
}
