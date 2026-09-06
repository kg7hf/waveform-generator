#include "usb_checkpoint.hpp"
#include "usb/usb_tinyusb_port.h"

#if defined(WFG_PLAYER_IMAGE)
#include "player.hpp"
#include "platform/waveform_msc.h"
#include "platform/wm8960_codec.hpp"
#include "status_contract.hpp"
#include "common/live_protocol.hpp"
#include "tx_artifact.hpp"
#include "tx_usb.hpp"
#include "fsl_common.h"
#endif

extern "C"
{
#include "FreeRTOS.h"
#include "task.h"
}

#include <cstdint>
#include <cstring>
#include <cstdio>
#include <string_view>

namespace waveform_generator
{

constexpr std::uint32_t usb_stack_words = 2048U;
StaticTask_t usb_task_control;
StackType_t usb_task_stack[usb_stack_words];
TaskHandle_t usb_task_handle{};

#if defined(WFG_PLAYER_IMAGE)
constexpr std::size_t control_line_capacity = live_protocol::wire_capacity;
constexpr std::size_t response_capacity = cdc_response_capacity;
char control_line[control_line_capacity]{};
std::size_t control_line_size{};
bool control_line_too_long{};
char response_buffer[response_capacity]{};
std::size_t response_size{};
std::size_t response_written{};
bool response_overflowed{};
bool json_fields{};
bool player_reply_waiting{};
bool legacy_reply{};
bool discard_player_reply{};
bool tx_reply_waiting{};
bool connected_before{};
std::uint32_t last_sequence{};
std::uint32_t response_sequence{};

void append_char(char value) noexcept
{
    if (response_size + 1U < response_capacity)
    {
        response_buffer[response_size] = value;
        response_size++;
    }
    else
    {
        response_overflowed = true;
    }
}

void append_literal(const char* value) noexcept
{
    while (*value != '\0')
    {
        append_char(*value);
        value++;
    }
}

void append_uint(std::uint64_t value) noexcept
{
    char digits[20]{};
    std::size_t digit_count = 0U;

    do
    {
        digits[digit_count] = static_cast<char>('0' + (value % 10U));
        digit_count++;
        value /= 10U;
    } while (value != 0U && digit_count < sizeof(digits));

    while (digit_count != 0U)
    {
        digit_count--;
        append_char(digits[digit_count]);
    }
}

void append_field(const char* name, std::uint64_t value) noexcept
{
    append_literal(json_fields ? ",\"" : " ");
    append_literal(name);
    append_literal(json_fields ? "\":" : "=");
    append_uint(value);
}

void finish_response() noexcept
{
    if (response_overflowed || response_size + 2U > response_capacity)
    {
        response_size = 0U;
        response_written = 0U;
        response_overflowed = false;
        append_literal("ERR RESPONSE TOO LONG");
    }

    response_buffer[response_size] = '\n';
    response_size++;
    response_buffer[response_size] = '\0';
    response_written = 0U;
}

void begin_response(const char* prefix) noexcept
{
    response_size = 0U;
    response_written = 0U;
    response_overflowed = false;
    append_literal(prefix);
}

void queue_error(const char* message) noexcept
{
    begin_response("ERR ");
    append_literal(message);
    finish_response();
}

void queue_simple_ok(const char* message) noexcept
{
    begin_response("OK ");
    append_literal(message);
    finish_response();
}


void begin_json(bool ok) noexcept
{
    const auto player = player_snapshot();
    const auto live = player_live_snapshot();
    begin_response("{\"seq\":"); append_uint(response_sequence);
    append_literal(",\"protocol\":\"WFG-LIVE/1\",\"ok\":"); append_literal(ok ? "true" : "false");
    append_literal(",\"state\":"); append_uint(player.state);
    append_literal(",\"run_id\":");
    if (live.run_id[0]) { append_char('"'); append_literal(live.run_id); append_char('"'); }
    else append_literal("null");
}

void queue_json_error(const char* code) noexcept
{
    begin_json(false);
    append_literal(",\"error\":{\"code\":\""); append_literal(code);
    append_literal("\",\"message\":\""); append_literal(code); append_literal("\"}}");
    finish_response();
}

void queue_status(const char* field = nullptr) noexcept
{
    const auto media = WFG_MediaGetSnapshot();
    const auto player = player_snapshot();
    const auto& codec = m110::imxrt1170::wm8960_codec();

    if (!field) begin_response("OK STATUS");
    else
    {
        begin_json(true);
        append_literal(",\""); append_literal(field); append_literal("\":{\"format\":\"engineering-counters\"");
        json_fields = true;
    }
    append_field("media_state", media.state);
    append_field("media_error", media.error);
    append_field("card_ready", media.card_ready);
    append_field("msc_ready", media.msc_ready);
    append_field("msc_read_only", media.msc_read_only);
    append_field("block_count", media.block_count);
    append_field("block_size", media.block_size);
    append_field("init_attempts", media.init_attempts);
    append_field("host_init_status", media.host_init_status);
    append_field("host_detect_status", media.host_detect_status);
    append_field("card_init_status", media.card_init_status);
    append_field("disk_init_status", media.disk_init_status);
    append_field("test_ready_calls", media.test_ready_calls);
    append_field("not_ready_responses", media.not_ready_responses);
    append_field("read_calls", media.read_calls);
    append_field("read_blocks", media.read_blocks);
    append_field("write_calls", media.write_calls);
    append_field("write_blocks", media.write_blocks);
    append_field("read_errors", media.read_errors);
    append_field("write_errors", media.write_errors);
    append_field("invalid_requests", media.invalid_requests);
    append_field("sync_cache_calls", media.sync_cache_calls);
    append_field("eject_requests", media.eject_requests);
    append_field("ownership_handoffs", media.ownership_handoffs);
    append_field("cd_gpio3_level", media.cd_gpio3_level);
    append_field("cd_cm7_gpio3_level", media.cd_cm7_gpio3_level);
    append_field("cd_inserted", media.cd_inserted);
    append_field("player_state", player.state);
    append_field("player_error", player.error);
    append_field("data_bytes", player.data_bytes);
    append_field("file_frames_total", player.file_frames_total);
    append_field("file_frames_enqueued", player.file_frames_enqueued);
    append_field("ring_fill_frames", player.ring_fill_frames);
    append_field("ring_min_frames", player.ring_min_frames);
    append_field("ring_min_pre_eof_frames", player.ring_min_pre_eof_frames);
    append_field("ring_max_frames", player.ring_max_frames);
    append_field("frames_requested", player.frames_requested);
    append_field("file_frames_submitted", player.file_frames_submitted);
    append_field("pcm_drained", player.pcm_drained);
    append_field("eof_count", player.eof_count);
    append_field("eof_silence_frames", player.eof_silence_frames);
    append_field("sd_read_calls", player.sd_read_calls);
    append_field("sd_bytes_read", player.sd_bytes_read);
    append_field("sd_short_reads", player.sd_short_reads);
    append_field("sd_read_errors", player.sd_read_errors);
    append_field("underruns", player.underruns);
    append_field("first_underrun_frame", player.first_underrun_frame);
    append_field("max_sd_read_cycles", player.max_sd_read_cycles);
    append_field("audio_running", codec.is_running() ? 1U : 0U);
    append_field("audio_blocks", codec.blocks_processed());
    append_field("audio_overruns", codec.rx_overruns());
    append_field("audio_rx_errors", codec.rx_errors());
    append_field("audio_tx_errors", codec.tx_errors());
    append_field("audio_max_backlog_blocks", codec.max_backlog_blocks());
    append_field("audio_max_block_cycles", codec.max_block_cycles());
    append_field("engine_state", player.engine_state);
    append_field("engine_error", player.engine_error);
    append_field("engine_stages", player.engine_stages);
    append_field("engine_frames_in", player.engine_frames_in);
    append_field("engine_frames_out", player.engine_frames_out);
    append_field("engine_clipped", player.engine_clipped);
    append_field("engine_digest_hi", player.engine_digest_hi);
    append_field("engine_digest_lo", player.engine_digest_lo);
    append_field("engine_source_digest_hi", player.engine_source_digest_hi);
    append_field("engine_source_digest_lo", player.engine_source_digest_lo);
    append_field("engine_max_block_cycles", player.engine_max_block_cycles);
    append_field("engine_events_applied", player.engine_events_applied);
    append_field("engine_events_dropped", player.engine_events_dropped);
    append_field("engine_arena_bytes", player.engine_arena_bytes);
    append_field("ring_capacity_frames", player.ring_capacity_frames);
    append_field("ring_high_watermark_frames", player.ring_high_watermark_frames);
    append_field("ring_wake_watermark_frames", player.ring_wake_watermark_frames);
    append_field("ring_critical_frames", player.ring_critical_frames);
    append_field("ring_avg_frames", player.ring_avg_frames);
    append_field("ring_critical_events", player.ring_critical_events);
    append_field("producer_rate_sps", player.producer_rate_sps);
    append_field("producer_worst_block_cycles", player.producer_worst_block_cycles);
    append_field("producer_sleeps", player.producer_sleeps);
    if (field)
    {
        const auto live = player_live_snapshot();
        append_field("live_seed", live.seed);
        append_field("live_frame", live.live_frame);
        append_field("live_digest_hi", live.live_digest >> 32);
        append_field("live_digest_lo", live.live_digest & UINT32_MAX);
        append_field("live_clipped", live.live_clipped);
        append_field("live_events", live.live_events);
        append_field("live_queue_free", live.queue_free);
        append_field("live_capture_free", live.capture_free);
        const auto artifact = tx_artifact_snapshot();
        append_field("tx_artifact_state", artifact.state);
        append_field("tx_payload_bytes", artifact.payload_bytes);
        append_field("tx_frames_written", artifact.frames_written);
        append_field("tx_total_frames", artifact.total_frames);
        append_literal(",\"tx_filename\":\""); append_literal(artifact.filename); append_literal("\"");
        append_literal(",\"tx_wav_sha256\":\""); append_literal(artifact.wav_sha256); append_literal("\"");
        append_literal(",\"tx_error\":\""); append_literal(artifact.error); append_literal("\"");
        append_literal(",\"live_reference_rms\":");
        char number[32]{};
        if (live.reference_rms > 0) { std::snprintf(number, sizeof(number), "%.17g", live.reference_rms); append_literal(number); }
        else append_literal("null");
        append_literal(",\"selected_file\":\""); append_literal(live.selected_file); append_literal("\"}}");
        json_fields = false;
    }
    finish_response();
}


void queue_info() noexcept
{
    begin_json(true);
    append_literal(",\"info\":{\"kind\":\"waveform-player\",\"board\":\"MIMXRT1170-EVK\",\"firmware_version\":\"0.2.0\",\"sample_rate_hz\":48000,\"usb_serial\":\"");
    constexpr char hex[] = "0123456789ABCDEF";
    const std::uint32_t words[] = {OCOTP->FUSEN[1].FUSE, OCOTP->FUSEN[2].FUSE};
    for (const auto word : words) for (int i = 7; i >= 0; --i) append_char(hex[(word >> (4 * i)) & 15]);
    append_literal("\",\"source_manifest_sha256\":\"");
    append_literal(WFG_SOURCE_MANIFEST_SHA256);
    append_literal("\",\"capabilities\":[\"wav-files\",\"live-controls\",\"sample-indexed-replay\",\"stepped-sweeps\",\"reusable-playback\"");
#if !defined(WFG_MSC_READ_ONLY)
    append_literal(",\"sd-wav-generation\",\"dd008-payload-upload\"],\"encoders\":[\"M110B\"]");
#else
    append_literal("],\"encoders\":[],\"read_only\":true");
#endif
    append_literal(",\"qualification\":false,\"request_capacity\":192,\"response_capacity\":4096}}");
    finish_response();
}

void handle_control_line() noexcept
{
    control_line[control_line_size] = '\0';
    std::string_view line(control_line, control_line_size);
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    // Preserve the original checkpoint tools. All new functionality uses sequences.
    if (line == "STATUS") { queue_status(); return; }
    if (line == "MEDIA HOST" || line == "MEDIA LOCAL")
    {
        if (line == "MEDIA HOST")
        {
            if (!player_allows_media_host()) queue_error("BUSY");
            else
            {
                const auto result = WFG_MediaRequestHost();
                if (result == kWFG_MediaHostRequestReady) queue_simple_ok("MEDIA HOST");
                else if (result == kWFG_MediaHostRequestInitializing) queue_simple_ok("MEDIA HOST INIT");
                else queue_error("MEDIA HOST FAILED");
            }
        }
        else if (WFG_MediaSetLocal()) queue_simple_ok("MEDIA LOCAL");
        else queue_error("MEDIA LOCAL FAILED");
        return;
    }
    live_protocol::Request command{};
    legacy_reply = line == "PLAY";
    if (legacy_reply) command.kind = live_protocol::Kind::play;
    else if (!live_protocol::parse(std::string_view(control_line, control_line_size), last_sequence, command))
    {
        response_sequence = command.seq;
        queue_json_error("BAD_REQUEST");
        return;
    }
    response_sequence = command.seq;
    using live_protocol::Kind;
    if (command.kind == Kind::generate_file)
    {
        tx_protocol::Request request{};
        request.kind = tx_protocol::Kind::generate_file;
        request.rate = static_cast<std::uint16_t>(command.rate);
        request.interleave = static_cast<std::uint8_t>(command.interleave);
        std::strcpy(request.name, command.name);
        std::strcpy(request.input_name, command.input_name);
        std::strcpy(request.encoder, command.encoder);
        std::strcpy(request.profile, command.profile);
        if (!submit_tx_request(request)) queue_json_error("BUSY");
        else tx_reply_waiting = true;
        return;
    }
    if (command.kind == Kind::info) { queue_info(); return; }
    if (command.kind == Kind::status || command.kind == Kind::counters)
    {
        queue_status(command.kind == Kind::status ? "status" : "counters");
        return;
    }
    if (command.kind == Kind::media_host || command.kind == Kind::media_local)
    {
        bool ok = false;
        if (command.kind == Kind::media_host)
        {
            if (player_allows_media_host()) ok = WFG_MediaRequestHost() != kWFG_MediaHostRequestFailed;
        }
        else ok = WFG_MediaSetLocal();
        if (!ok) { queue_json_error("BUSY"); return; }
        begin_json(true); append_literal(",\"result\":{\"accepted\":true}}"); finish_response();
        return;
    }
    if (!submit_player_command(command))
    {
        if (legacy_reply) queue_error("BUSY"); else queue_json_error("BUSY");
        return;
    }
    player_reply_waiting = true;
}

void service_response() noexcept
{
    while (response_written < response_size)
    {
        const auto written = m110_tinyusb_cdc_write(
            response_buffer + response_written,
            static_cast<std::uint32_t>(response_size - response_written));
        if (written == 0U)
        {
            break;
        }
        response_written += written;
    }

    m110_tinyusb_cdc_flush();
    if (response_written == response_size)
    {
        response_size = 0U;
        response_written = 0U;
    }
}

void service_control() noexcept
{
    const bool connected = m110_tinyusb_cdc_connected();
    if (connected_before && !connected)
    {
        last_sequence = 0;
        control_line_size = 0;
        control_line_too_long = false;
        response_size = response_written = 0;
        if (player_reply_waiting) discard_player_reply = true;
        if (tx_usb_active() || tx_reply_waiting) tx_usb_disconnect();
        tx_reply_waiting = false;
    }
    connected_before = connected;
    if (tx_usb_active())
    {
        tx_usb_service();
        while (m110_tinyusb_cdc_available() && !tx_usb_blocked())
        {
            std::uint8_t byte{};
            if (m110_tinyusb_cdc_read(&byte, 1) != 1) break;
            tx_usb_feed(byte);
        }
        tx_usb_service();
        return;
    }
    if (tx_reply_waiting)
    {
        tx_protocol::Reply reply{};
        if (!take_tx_reply(reply)) return;
        tx_reply_waiting = false;
        if (!reply.ok) queue_json_error(reply.error);
        else
        {
            begin_json(true);
            append_literal(",\"result\":{\"accepted\":true,\"generating\":true}}");
            finish_response();
        }
    }
    if (player_reply_waiting)
    {
        ControlReply reply{};
        if (!take_player_reply(reply)) return;
        player_reply_waiting = false;
        if (discard_player_reply) { discard_player_reply = false; return; }
        if (legacy_reply)
        {
            if (reply.ok) queue_simple_ok("PLAY"); else queue_error(reply.error);
        }
        else if (!reply.ok) queue_json_error(reply.error);
        else
        {
            begin_json(true);
            append_literal(",\"result\":{\"accepted\":true,\"apply_frame\":"); append_uint(reply.apply_frame);
            append_literal(",\"events\":"); append_uint(reply.events); append_literal("}}");
            finish_response();
        }
    }
    if (response_size) { service_response(); return; }
    while (m110_tinyusb_cdc_available() && !response_size && !player_reply_waiting && !tx_reply_waiting)
    {
        char byte{};
        if (m110_tinyusb_cdc_read(&byte, 1) != 1) break;
        // DD008 starts with a NUL resynchronizer, then COBS HELLO code 0x05.
        // Bind one protocol per CDC
        // connection; switch by closing/reopening the port, never mid-record.
        if (!control_line_size && !last_sequence && (byte == 0 || static_cast<unsigned char>(byte) == 5U))
        {
            tx_usb_feed(static_cast<std::uint8_t>(byte));
            return;
        }
        if (control_line_too_long)
        {
            if (byte == '\n')
            {
                control_line_too_long = false;
                control_line_size = 0;
                response_sequence = 0;
                queue_json_error("LINE_TOO_LONG");
            }
            continue;
        }
        if (byte == '\n') { handle_control_line(); control_line_size = 0; }
        else if (control_line_size + 1 < control_line_capacity) control_line[control_line_size++] = byte;
        else { control_line_too_long = true; control_line_size = 0; }
    }
    if (response_size) service_response();
}

#endif

void usb_task_entry(void*) noexcept
{
    if (!m110_tinyusb_stack_initialize())
    {
        vTaskSuspend(nullptr);
    }

    for (;;)
    {
        m110_tinyusb_task(10U);
#if defined(WFG_PLAYER_IMAGE)
        service_control();
#endif
    }
}

m110::Status start_usb_checkpoint() noexcept
{
    if (usb_task_handle != nullptr)
    {
        return {m110::StatusCode::invalid_argument, "USB checkpoint already started"};
    }

    if (!m110_usb_hardware_initialize())
    {
        return {m110::StatusCode::unavailable, "USB hardware initialization failed"};
    }

    usb_task_handle = xTaskCreateStatic(&usb_task_entry, "wfg_usb", usb_stack_words,
                                      nullptr, 3U, usb_task_stack, &usb_task_control);
    if (usb_task_handle == nullptr)
    {
        return {m110::StatusCode::unavailable, "USB checkpoint task creation failed"};
    }

    return m110::Status::success();
}

} // namespace waveform_generator
