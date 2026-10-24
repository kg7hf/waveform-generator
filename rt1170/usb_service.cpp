// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

#include "platform/device_identity.h"

#include "c_status_map.hpp"
#include "usb/usb_tinyusb_port.h"
#include "usb_service.hpp"

#if defined(WFG_PLAYER_IMAGE)
#include "card_listing.hpp"
#include "common/live_protocol.hpp"
#include "error_map.hpp"
#include "fsl_common.h"
#include "os/critical_section.hpp"
#include "platform/waveform_msc.h"
#include "platform/waveform_sd_clock.h"
#include "platform/wm8960_codec.hpp"
#include "player.hpp"
#include "status_contract.hpp"
#include "tx_artifact.hpp"
#include "tx_usb.hpp"
#include "usb/usb_composite.h"
#include "usb_audio.hpp"
#include "usb_ep0_order.hpp"
#include "waveform-source/encoder_registry.hpp"
#include "wfg_build_info.hpp"
#endif

extern "C"
{
#include "FreeRTOS.h"
#include "task.h"
}

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

namespace waveform_generator
{

constexpr std::uint32_t usb_stack_words = 2048U;
StaticTask_t usb_task_control;
StackType_t usb_task_stack[usb_stack_words];
TaskHandle_t usb_task_handle{};

#if defined(WFG_PLAYER_IMAGE)
constexpr std::size_t control_line_capacity = live_protocol::wire_capacity;
constexpr std::size_t response_capacity = cdc_response_capacity;

// An unsequenced diagnostic line that the player task answers; its OK reply
// echoes the line.
struct LegacyPlayerLine
{
    std::string_view text;
    live_protocol::Kind kind;
};

constexpr auto legacy_player_lines = std::to_array<LegacyPlayerLine>({
    {.text = "PLAY", .kind = live_protocol::Kind::play},
    {.text = "OUTPUT CODEC", .kind = live_protocol::Kind::output_codec},
    {.text = "OUTPUT USB", .kind = live_protocol::Kind::output_usb},
});

std::size_t control_line_size{};
bool control_line_too_long{};
char response_buffer[response_capacity]{};
std::size_t response_size{};
std::size_t response_written{};
bool response_overflowed{};
bool json_fields{};
bool player_reply_waiting{};
bool pending_files_reply{};
// The legacy line whose reply the player task owes, or none for a WFG-LIVE/1
// request. USB-task state: handle_control_line() sets it, service_control()
// answers with it.
std::optional<LegacyPlayerLine> pending_legacy_line{};
bool discard_player_reply{};
bool tx_reply_waiting{};
bool connected_before{};
std::uint32_t last_sequence{};
std::uint32_t response_sequence{};

// The request line being received, in OCRAM2 (.ocram, NOLOAD: startup never
// writes it, so its initializer is not loaded): control_line_size counts its
// bytes, and each is written before it is read. A CUT request carries a whole
// schedule (live_protocol::wire_capacity), so it stays out of DTCM.
[[nodiscard]] std::span<char, control_line_capacity> control_line() noexcept
{
    __attribute__((section(".ocram.control_line"))) static constinit std::array<char, control_line_capacity> line{};
    return line;
}

// The line received so far.
[[nodiscard]] std::string_view received_line() noexcept
{
    const auto received = control_line().first(control_line_size);
    return {received.data(), received.size()};
}

// The CMake build label, typed once where it enters the protocol.
constexpr std::string_view firmware_build_id{WFG_BUILD_ID};
// STATUS strings stay within the widths status_json_worst_case_bytes() counts.
static_assert(firmware_build_id.size() <= build_id_max_chars);
static_assert(build_info::fw_commit.size() <= fw_commit_max_chars);
// The generated identity is a full commit name or unknown, never a partial one,
// and a build that cannot name its commit cannot know its dirty state either.
static_assert(is_commit_id(build_info::fw_commit) || build_info::fw_commit == unknown_commit);
static_assert(build_info::fw_commit != unknown_commit || build_info::fw_dirty == FwDirty::unknown);
static_assert(sizeof(LiveSnapshot::run_id) - 1U == run_id_max_chars);
static_assert(sizeof(LiveSnapshot::selected_file) - 1U == short_file_name_max_chars);
static_assert(sizeof(TxArtifactSnapshot::filename) - 1U == short_file_name_max_chars);
static_assert(sizeof(TxArtifactSnapshot::wav_sha256) - 1U == sha256_hex_chars);
// The PID this image enumerates with (usb/usb_descriptors.c); INFO reports it.
constexpr std::uint16_t usb_product_id{wfg_usb_product_id_player_image};
// Whether the image offers the 16-bit mono capture alternate (WFG_UAC_MONO_ALT);
// INFO lists the usb-audio-mono capability exactly then.
constexpr bool usb_audio_mono_offered{usb_audio::offered_format(usb_audio::pcm16_mono.alternate).has_value()};
// Whether the image clocks the SD bus from BOARD_BootClockRUN's 24 MHz root
// (WFG_SD_CLOCK_24MHZ) instead of at 49.5 MHz; INFO lists the sd-clock-24mhz
// capability exactly then.
constexpr bool sd_clock_24mhz{wfg_sd_image_bus_hz == wfg_sd_boot_bus_hz};

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

void append_text(std::string_view value) noexcept
{
    for (const char character : value)
    {
        append_char(character);
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

void append_json_number(double value) noexcept
{
    if (!std::isfinite(value))
    {
        append_literal("null");
        return;
    }

    char number[32]{};
    const auto written = std::snprintf(number, sizeof(number), "%.17g", value);

    if (written <= 0 || static_cast<std::size_t>(written) >= sizeof(number) || number[0] == '\0')
    {
        append_literal("null");
        return;
    }

    append_literal(number);
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

void queue_simple_ok(std::string_view message) noexcept
{
    begin_response("OK ");
    append_text(message);
    finish_response();
}

void begin_json(bool ok) noexcept
{
    const auto player = player_snapshot();
    const auto live = player_live_snapshot();
    begin_response("{\"seq\":");
    append_uint(response_sequence);
    append_literal(",\"protocol\":\"WFG-LIVE/1\",\"ok\":");
    append_literal(ok ? "true" : "false");
    append_literal(",\"state\":");
    append_uint(player.state);
    append_literal(",\"run_id\":");

    if (live.run_id[0])
    {
        append_char('"');
        append_literal(live.run_id);
        append_char('"');
    }
    else
    {
        append_literal("null");
    }
}

void queue_json_error(const char* code) noexcept
{
    begin_json(false);
    append_literal(",\"error\":{\"code\":\"");
    append_literal(code);
    append_literal("\",\"message\":\"");
    append_literal(code);
    append_literal("\"}}");
    finish_response();
}

// A refused player command's JSON error. A refused PLAYLIST start names the
// playlist file's line in its message, and what the card check found there:
// "<code> line <n>: <detail>" (either part only when there is one).
void queue_reply_error(const ControlReply& reply) noexcept
{
    if (reply.line == 0U && reply.detail == nullptr)
    {
        queue_json_error(reply.error);
        return;
    }

    begin_json(false);
    append_text(R"(,"error":{"code":")");
    append_literal(reply.error);
    append_text(R"(","message":")");
    append_literal(reply.error);

    if (reply.line != 0U)
    {
        append_text(" line ");
        append_uint(reply.line);
    }

    if (reply.detail != nullptr)
    {
        append_text(": ");
        append_literal(reply.detail);
    }

    append_text(R"("}})");
    finish_response();
}

// The sound-card stream, the current or last play on the USB route,
// TinyUSB's assertion failures, and the current or last play's cuts (on
// either route).
struct UacStatus
{
    usb_audio::StreamCounters stream;
    usb_audio::PlayStatistics play;
    usb_audio::StackAsserts asserts;
    PlayCutStatistics cuts;
};

// The stream and the play belong to the USB interrupt level, so they are read
// together in one critical section as the reply is formed: uac_stream_frames
// is then the exact count of frames sent at that moment, consistent with the
// play statistics. The assertion record joins them, so a reply's assert count
// and the latest assert's frame index are of the same moment, and so do the
// play's cuts, counted within the frames that play statistics confirm.
[[nodiscard]] UacStatus uac_status() noexcept
{
    const TaskCriticalSection lock;
    const auto play = usb_audio::play_statistics();
    return {.stream = usb_audio::stream_counters(), .play = play, .asserts = usb_audio::stack_asserts(), .cuts = player_cut_statistics(play)};
}

// The capture stream's diagnostics, which STATUS and USB? both report.
void append_uac_stream_fields(const UacStatus& uac) noexcept
{
    const auto& stream = uac.stream;
    const auto host = usb_audio::host_settings();
    append_field("uac_stream_open", stream.state == usb_audio::StreamState::closed ? 0U : 1U);
    append_field("uac_stream_opens", stream.opens);
    append_field("uac_stream_frames", stream.frames_sent.value);
    append_field("uac_short_packets", stream.short_packets);
    append_field("uac_missed_intervals", stream.missed_intervals.value);
    // The raw UAC2 volume (int16, 1/256 dB) as its 16-bit two's complement: 0 is 0 dB.
    append_field("uac_host_volume", std::bit_cast<std::uint16_t>(host.volume.value));
    append_field("uac_host_mute", host.muted ? 1U : 0U);
    append_field("usb_stack_asserts", uac.asserts.count);
}

void append_uac_fields() noexcept
{
    const auto uac = uac_status();
    append_uac_stream_fields(uac);
    // The engine output frames the play's sent frames cover (a clean card's
    // own frames): the frames themselves without cuts; with them, less the
    // zeros sent, plus the frames dropped.
    append_field("uac_play_file_frames", uac.cuts.usb_file_frames);
    append_field("uac_play_crc32_l", uac.play.crc32_left);
    append_field("uac_play_crc32_r", uac.play.crc32_right);
    append_field("uac_play_first_frame_index", uac.play.first_frame_index.value);
    append_field("uac_play_first_frame_sof", uac.play.first_frame_sof.value);
    append_field("uac_play_last_frame_index", uac.play.last_frame_index.value);
    append_field("uac_play_accept_frame_index", uac.play.accept_frame_index.value);
    append_field("uac_play_missed_intervals", uac.play.missed_intervals.value);
    append_field("uac_play_first_miss_frame_index", uac.play.first_miss_frame_index.value);
}

void queue_status(const char* field = nullptr) noexcept
{
    const auto media = WFG_MediaGetSnapshot();
    const auto player = player_snapshot();
    const auto& codec = m110::imxrt1170::wm8960_codec();

    if (!field)
    {
        begin_response("OK STATUS");
    }
    else
    {
        begin_json(true);
        append_literal(",\"");
        append_literal(field);
        append_literal("\":{\"format\":\"engineering-counters\"");
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
    append_field("audio_tx_races", codec.tx_races());
    append_field("audio_tx_fifo_words_at_start", codec.tx_fifo_words_at_start());
    append_field("audio_rx_fifo_words_at_start", codec.rx_fifo_words_at_start());
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
    append_field("fw_commit_hi", build_info::fw_commit_hi);
    append_field("fw_commit_lo", build_info::fw_commit_lo);
    append_field("fw_dirty", std::to_underlying(build_info::fw_dirty));
    append_field("output_route", std::to_underlying(player_output_route()));
    append_field("msc_ejected", media.msc_ejected);
    append_field("msc_host_hold", media.msc_host_hold);
    append_uac_fields();

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
        append_literal(",\"tx_filename\":\"");
        append_literal(artifact.filename);
        append_literal("\"");
        append_literal(",\"tx_wav_sha256\":\"");
        append_literal(artifact.wav_sha256);
        append_literal("\"");
        append_literal(",\"tx_error\":\"");
        append_text(std::string_view{artifact.error}.substr(0U, tx_error_max_chars));
        append_literal("\"");
        append_literal(",\"live_reference_rms\":");

        if (live.reference_rms > 0)
        {
            append_json_number(live.reference_rms);
        }
        else
        {
            append_literal("null");
        }

        append_literal(",\"selected_file\":\"");
        append_literal(live.selected_file);
        append_literal("\",\"build_id\":\"");
        append_text(firmware_build_id);
        append_literal("\",\"fw_commit\":\"");
        append_text(build_info::fw_commit);
        append_literal("\"}}");
        json_fields = false;
    }

    finish_response();
}

// The current or last play's CUT injector, last in USB?. Apart from the
// stream's and the play's missed intervals, which cuts never touch.
void append_play_cut_fields(const PlayCutStatistics& cuts) noexcept
{
    append_field("uac_play_cut_count", cuts.applied);
    append_field("uac_play_cut_drop_frames", cuts.drop_frames);
    append_field("uac_play_cut_zero_frames", cuts.zero_frames);
    append_field("uac_play_cut_first_index", first_cut_wire_index(cuts.first));
    append_field("uac_play_cut_rejected", cuts.rejected);
    append_field("uac_play_sent_frames", cuts.sent_frames);
}

// USB?: the capture stream's diagnostics as STATUS reports them, then the ones
// STATUS has no room for: lost chunks, the stream's miss attribution, the
// play's unprimed part, where TinyUSB's assertions failed and the capture
// format of the stream and of the play, read together as STATUS reads them,
// the MSC transfer maxima and the SD bus clock of the media layer, the play's
// cuts, its engine deadline misses, the USB interrupt's EP0 order, and last
// the completions reported short of their packet (status_contract.hpp
// usb_field_names, usb_json_worst_case_bytes()).
void queue_usb_diagnostics() noexcept
{
    const auto uac = uac_status();
    const auto media = WFG_MediaGetSnapshot();
    const auto ep0 = usb_ep0_order::ep0_order_counters();
    const auto& stream = uac.stream;
    const auto& asserts = uac.asserts;
    const auto& play_format = uac.play.format;
    begin_json(true);
    append_text(usb_json_object_header);
    json_fields = true;
    append_uac_stream_fields(uac);
    append_field("uac_rejected_chunks", stream.rejected_chunks);
    append_field("uac_last_miss_frame_index", stream.last_miss.frame_index.value);
    append_field("uac_last_miss_unprimed", stream.last_miss.unprimed.value);
    append_field("uac_last_miss_play", stream.last_miss.play ? 1U : 0U);
    append_field("uac_max_gap_frames", stream.max_gap.value);
    append_field("uac_watchdog_expiries", stream.watchdog_expiries);
    append_field("uac_unprimed_intervals", stream.unprimed_intervals.value);
    append_field("uac_play_unprimed_intervals", uac.play.unprimed_intervals.value);
    append_field("usb_assert_count", asserts.count);
    append_field("usb_first_assert_pc", asserts.first_pc.value);
    append_field("usb_last_assert_pc", asserts.last_pc.value);
    append_field("usb_last_assert_frame_index", asserts.last_frame_index.value);
    append_field("usb_last_assert_tick", asserts.last_tick.value);
    append_field("uac_stream_alt", stream.format.alternate);
    append_field("uac_stream_bits", stream.format.bits);
    append_field("uac_stream_channels", stream.format.channels);
    append_field("uac_play_stream_bits", play_format.bits);
    append_field("uac_play_stream_channels", play_format.channels);
    append_field("msc_max_write_us", media.msc_max_write_us);
    append_field("msc_max_read_us", media.msc_max_read_us);
    append_field("msc_max_blocks_per_call", media.msc_max_blocks_per_call);
    append_field("sd_bus_clock_hz", media.sd_bus_clock_hz);
    append_play_cut_fields(uac.cuts);
    append_field("engine_deadline_misses", player_deadline_misses());
    append_field("usb_ep0_reordered", ep0.reordered);
    append_field("usb_ep0_unordered", ep0.unordered);
    append_field("uac_sent_mismatches", stream.sent_mismatches);
    json_fields = false;
    append_text("}}");
    finish_response();
}

// The characters below the space, which a JSON string may not hold as they are.
constexpr unsigned char first_printable = 0x20U;

// A JSON string's characters: a quote and a backslash escaped, a control
// character (no static message has one) as '?'. char is unsigned on the
// target and signed on the host, so a character is compared as its byte.
void append_json_escaped(std::string_view text) noexcept
{
    for (const char character : text)
    {
        if (character == '"' || character == '\\')
        {
            append_char('\\');
            append_char(character);
        }
        else
        {
            append_char(std::bit_cast<unsigned char>(character) < first_printable ? '?' : character);
        }
    }
}

// The reason ENGINE? gives: why the live controls stopped the play when they
// did (player_error live_control) and said why, even after a refused
// scenario; else why its scenario was refused or its engine stopped; else the
// live controls' (success: none).
[[nodiscard]] constexpr m110::Status engine_diagnostics_reason(std::uint32_t player_error, m110::Status engine, m110::Status live) noexcept
{
    const bool live_stopped = player_error == std::to_underlying(PlayerError::live_control) && !live.is_ok();
    return live_stopped || engine.is_ok() ? live : engine;
}

// A scenario's refusal and a live fault, for the checks below.
constexpr m110::Status refused_scenario_sample{.code = m110::StatusCode::invalid_argument, .message = "a refused scenario"};
constexpr m110::Status live_fault_sample{.code = m110::StatusCode::invalid_configuration, .message = "a live fault"};
constexpr auto live_control_error = std::to_underlying(PlayerError::live_control);
constexpr auto no_player_error = std::to_underlying(PlayerError::none);

static_assert(engine_diagnostics_reason(live_control_error, refused_scenario_sample, live_fault_sample).message == live_fault_sample.message &&
                  engine_diagnostics_reason(live_control_error, m110::Status::success(), live_fault_sample).message == live_fault_sample.message,
              "the live controls stopped the play: their reason, after a refused scenario too");
static_assert(engine_diagnostics_reason(no_player_error, refused_scenario_sample, live_fault_sample).message == refused_scenario_sample.message &&
                  engine_diagnostics_reason(live_control_error, refused_scenario_sample, m110::Status::success()).message == refused_scenario_sample.message &&
                  engine_diagnostics_reason(no_player_error, m110::Status::success(), m110::Status::success()).is_ok(),
              "otherwise the engine's reason, and none without one");

// The latched channel identity, not the mutable engine used by card preflight.
void append_watterson_identity(const playlist::WattersonIdentity& identity) noexcept
{
    append_field("watterson_version", identity.version);
    append_field("watterson_digest_hi", identity.digest_hi);
    append_field("watterson_digest_lo", identity.digest_lo);
    append_field("watterson_latency_frames", identity.latency_frames);
    append_field("watterson_paths", identity.paths);
}

void append_secondary_source_identity(const SecondarySourceIdentity& identity) noexcept
{
    append_field("second_source_version", identity.version);
    append_field("second_source_source_frames", identity.source_frames);
    append_field("second_source_excerpt_start_frame", identity.excerpt_start_frame);
    append_field("second_source_excerpt_frames", identity.excerpt_frames);
    append_field("second_source_digest_hi", identity.digest_hi);
    append_field("second_source_digest_lo", identity.digest_lo);
    append_field("second_source_latency_frames", identity.latency_frames);
    append_text(R"(,"second_source_reference_rms":)");
    append_json_number(identity.reference_rms);
}

void append_prepare_string(const char* name, std::string_view value) noexcept
{
    append_literal(",\"");
    append_text(name);
    append_literal("\":\"");
    append_json_escaped(value);
    append_char('"');
}

void queue_prepare_diagnostics() noexcept
{
    const auto snapshot = player_prepare_snapshot();
    begin_json(true);
    append_literal(",\"prepare\":{\"phase\":");
    append_uint(std::to_underlying(snapshot.phase));
    const std::string_view reason = snapshot.status.message == nullptr ? "unknown" : snapshot.status.message;
    append_prepare_string("error", reason.substr(0U, prepare_reason_max_chars));
    append_prepare_string("source", snapshot.request.source.data());
    append_prepare_string("scenario", snapshot.request.scenario.data());
    append_prepare_string("output", snapshot.request.output.data());
    append_prepare_string("retention", snapshot.request.retention == prepare::Retention::temporary ? "temporary" : "keep");
    json_fields = true;
    append_field("seed", snapshot.seed);
    append_field("input_frames", snapshot.input_frames);
    append_field("output_frames", snapshot.output_frames);
    append_field("total_input_frames", snapshot.total_input_frames);
    json_fields = false;
    append_prepare_string("wav_sha256", snapshot.wav_sha256.data());
    append_prepare_string("pcm_sha256", snapshot.pcm_sha256.data());
    append_literal("}}");
    finish_response();
}

void append_prepare_boolean(const char* name, bool value) noexcept
{
    append_literal(",\"");
    append_text(name);
    append_literal("\":");
    append_literal(value ? "true" : "false");
}

// PREPAREPLAY?: the player publishes one coherent copy after each owner step.
// Reading it performs no SD work or scheduler transitions. A final snapshot
// survives unrelated preparation so the host can collect the last outcome.
void queue_prepared_playback_diagnostics() noexcept
{
    const auto snapshot = player_prepared_playback_snapshot();
    const auto& progress = snapshot.progress;
    begin_json(true);
    append_literal(",\"prepare_play\":{\"state\":");
    append_uint(std::to_underlying(progress.state));
    json_fields = true;
    append_field("operation_id", snapshot.operation_id);
    append_field("rate", snapshot.measured_rate_frames_per_second);
    append_field("margin_frames", snapshot.margin_frames);
    append_field("total_frames", progress.total_frames);
    append_field("required_lead_frames", progress.required_lead_frames);
    append_field("produced_frames", progress.produced_frames);
    append_field("written_frames", progress.written_frames);
    append_field("committed_frames", progress.committed_frames);
    append_field("queued_frames", progress.read_frames);
    append_field("consumed_frames", progress.consumed_frames);
    append_field("status", std::to_underlying(snapshot.status.code));
    const auto& timing = snapshot.timing;
    append_literal(R"(,"timing":{"cycle_hz":)");
    append_uint(timing.cycle_hz);
    append_literal(R"(,"tick_hz":)");
    append_uint(timing.tick_hz);
    append_literal(R"(,"phases":{)");

    for (std::size_t index = 0U; index < timing.phases.size(); ++index)
    {
        if (index != 0U)
        {
            append_char(',');
        }

        const auto& stats = timing.phases[index];
        append_char('"');
        append_literal(prepared_timing_phase_names[index]);
        append_literal(R"(":{"calls":)");
        append_uint(stats.calls);
        append_field("total_cycles", stats.total_cycles);
        append_field("max_cycles", stats.max_cycles);
        append_field("rejected", stats.rejected);
        append_literal(R"(,"saturated":)");
        append_literal(stats.saturated ? "true" : "false");
        append_char('}');
    }

    append_literal("}}");
    json_fields = false;
    append_prepare_boolean("active", snapshot.active);
    append_prepare_boolean("started", progress.started);
    append_prepare_boolean("artifact_finalized", progress.artifact_finalized);
    append_prepare_boolean("action_pending", progress.action_pending);
    append_prepare_boolean("owner_released", progress.owner_released);
    const std::string_view reason = snapshot.status.message == nullptr ? "unknown" : snapshot.status.message;
    append_prepare_string("reason", reason.substr(0U, prepare_reason_max_chars));
    append_literal("}}");
    finish_response();
}

// ENGINE?: identity, state, error and reason read together under one lock.
// The reason has at most engine_reason_max_chars characters, escaped, or null
// while none. status_contract.hpp sizes every field at its widest.
void queue_engine_diagnostics() noexcept
{
    PlayerSnapshot player{};
    m110::Status reason{};
    playlist::WattersonIdentity identity{};
    SecondarySourceIdentity secondary_identity{};
    {
        const TaskCriticalSection lock;
        player = player_snapshot();
        identity = player_watterson_identity();
        secondary_identity = player_secondary_source_identity();
        reason = engine_diagnostics_reason(player.error, player_engine_reason(), player_live_reason());
    }

    begin_json(true);
    append_text(engine_json_object_header);
    json_fields = true;
    append_watterson_identity(identity);
    append_secondary_source_identity(secondary_identity);
    append_field("engine_state", player.engine_state);
    append_field("engine_error", player.engine_error);
    json_fields = false;
    append_text(R"(,"engine_reason":)");

    if (reason.is_ok() || reason.message == nullptr)
    {
        append_text("null");
    }
    else
    {
        append_char('"');
        append_json_escaped(std::string_view{reason.message}.substr(0U, engine_reason_max_chars));
        append_char('"');
    }

    append_text("}}");
    finish_response();
}

// A JSON string member, ,"name":"value": every value here is an 8.3 name, a
// label or a fixed word, none of which needs escaping.
void append_string_member(std::string_view name, std::string_view value) noexcept
{
    append_text(R"(,")");
    append_text(name);
    append_text(R"(":")");
    append_text(value);
    append_char('"');
}

// A string member that is null when empty: an item's label and scn=.
void append_optional_string_member(std::string_view name, std::string_view value) noexcept
{
    if (!value.empty())
    {
        append_string_member(name, value);
        return;
    }

    append_text(R"(,")");
    append_text(name);
    append_text(R"(":null)");
}

// A number member that is null when there is none: an item's seed= and its
// scheduled stream index.
void append_optional_field(std::string_view name, std::optional<std::uint32_t> value) noexcept
{
    append_text(R"(,")");
    append_text(name);
    append_text(R"(":)");

    if (value.has_value())
    {
        append_uint(*value);
        return;
    }

    append_text("null");
}

// A duration's frames, as PLAYLIST? reports dur=.
[[nodiscard]] constexpr std::uint32_t duration_frames(playlist::DurationFrames duration) noexcept
{
    return duration.value;
}

// One result block (status_contract.hpp playlist_block_fields): which item,
// how it ended, its per-play statistics under their STATUS and USB? names,
// when it played, its attenuation and duration, its loops, why it ended and
// its engine deadline misses.
void append_result_block(const playlist::ResultBlock& block) noexcept
{
    const auto& play = block.play;
    append_text(R"({"seq":)");
    append_uint(block.seq);
    append_field("run", block.run);
    append_field("item", block.item);
    append_optional_string_member("label", block.label.view());
    append_string_member("file", block.file.view());
    append_string_member("route", playlist::route_name(block.route));
    append_string_member("result", playlist::outcome_name(block.outcome));
    append_field("player_error", block.player_error);
    append_field("file_frames_total", play.file_frames_total);
    append_optional_string_member("scn", block.scenario.view());
    append_optional_field("seed", block.seed);
    append_field("scn_crc32", block.scenario_crc32);
    append_field("engine_state", play.engine_state);
    append_field("engine_error", play.engine_error);
    append_field("uac_play_file_frames", play.usb_file_frames);
    append_field("uac_play_sent_frames", play.sent_frames);
    append_field("uac_play_crc32_l", play.crc32_left);
    append_field("uac_play_crc32_r", play.crc32_right);
    append_field("uac_play_first_frame_index", play.first_frame_index);
    append_field("uac_play_last_frame_index", play.last_frame_index);
    append_field("uac_play_first_frame_sof", play.first_frame_sof);
    append_field("uac_play_missed_intervals", play.missed_intervals);
    append_field("uac_play_first_miss_frame_index", play.first_miss_frame_index);
    append_field("uac_play_stream_bits", play.stream_bits);
    append_field("uac_play_stream_channels", play.stream_channels);
    append_field("uac_play_cut_count", play.cut_count);
    append_field("uac_play_cut_drop_frames", play.cut_drop_frames);
    append_field("uac_play_cut_zero_frames", play.cut_zero_frames);
    append_field("uac_play_cut_first_index", play.cut_first_index);
    append_field("uac_play_cut_rejected", play.cut_rejected);
    append_field("underruns", play.underruns);
    append_field("pcm_drained", play.pcm_drained);
    append_field("gap_frames", block.gap.value);
    append_optional_field("scheduled_index", block.scheduled_index);
    append_field("start_late", block.start_late() ? 1U : 0U);
    append_field("start_tick", block.start_tick);
    append_field("end_tick", block.end_tick);
    append_field("att_tenths_db", block.attenuation.value());
    append_optional_field("dur_frames", block.duration.transform(duration_frames));
    append_field("loops", block.loops);
    append_string_member("end_cause", playlist::end_cause_name(block.end_cause));
    append_field("engine_deadline_misses", play.deadline_misses);
    append_watterson_identity(play.watterson);
    append_secondary_source_identity(play.secondary_source);
    append_field("engine_digest_hi", play.engine_digest_hi);
    append_field("engine_digest_lo", play.engine_digest_lo);
    append_char('}');
}

// The PLAYLIST? reply of `snapshot` (status_contract.hpp
// playlist_progress_fields, playlist_block_fields,
// playlist_json_worst_case_bytes()).
void queue_playlist_reply(const playlist::Snapshot& snapshot) noexcept
{
    const auto& progress = snapshot.progress;
    begin_json(true);
    append_text(playlist_json_object_header);
    json_fields = true;
    append_string_member("state", playlist::state_name(progress.state));
    append_string_member("name", progress.name.view());
    append_field("run", progress.run);
    append_string_member("policy", playlist::policy_name(progress.policy));
    append_field("items", progress.items);
    append_field("index", progress.index);
    append_field("completed", progress.completed);
    append_field("failed", progress.failed);
    append_field("pending", snapshot.pending);
    append_field("oldest_seq", snapshot.oldest_seq);
    append_field("next_seq", snapshot.next_seq);
    append_field("tick", snapshot.tick);
    append_text(playlist_blocks_open);
    bool first = true;

    for (const auto& block : std::span{snapshot.blocks}.first(std::min(snapshot.block_count, snapshot.blocks.size())))
    {
        if (!first)
        {
            append_char(',');
        }

        first = false;
        append_result_block(block);
    }

    append_text(playlist_blocks_close);
    json_fields = false;
    append_text("}}");
    finish_response();
}

// PLAYLIST?: the running or last playlist's progress, the result ring's
// numbers and its oldest blocks, read in one critical section.
void queue_playlist_status() noexcept
{
    queue_playlist_reply(player_playlist_snapshot());
}

// FILES? returns one bounded card-listing page produced by the player task.
// The USB task only serializes the borrowed page; it never opens or reads the card.
void queue_files(const card_listing::Page& page) noexcept
{
    if (page.count > card_listing::page_capacity || page.offset > page.total || static_cast<std::uint64_t>(page.count) > page.total - page.offset || (page.count == 0U && page.offset < page.total))
    {
        queue_json_error(refusal::files_page.message);
        return;
    }

    for (std::uint32_t index = 0U; index < page.count; ++index)
    {
        std::size_t length = 0U;

        while (length < card_listing::name_capacity && page.entries[index].name[length] != '\0')
        {
            ++length;
        }

        if (length == 0U || length == card_listing::name_capacity)
        {
            queue_json_error(refusal::files_page.message);
            return;
        }
    }

    begin_json(true);
    append_literal(R"(,"files":{"offset":)");
    append_uint(page.offset);
    append_literal(R"(,"total":)");
    append_uint(page.total);
    const std::uint64_t next_offset = page.offset + page.count;
    append_literal(R"(,"next_offset":)");

    if (next_offset < page.total)
    {
        append_uint(next_offset);
    }
    else
    {
        append_literal("null");
    }

    append_literal(R"(,"entries":[)");

    for (std::uint32_t index = 0U; index < page.count; ++index)
    {
        if (index != 0U)
        {
            append_char(',');
        }

        const auto& entry = page.entries[index];
        std::size_t length = 0U;

        while (length < card_listing::name_capacity && entry.name[length] != '\0')
        {
            ++length;
        }

        append_literal(R"({"name":")");
        append_json_escaped(std::string_view{entry.name, length});
        append_literal(R"(","size_bytes":)");
        append_uint(entry.size_bytes);
        append_char('}');
    }

    append_literal("]}}");
    finish_response();
}

void queue_player_reply(const ControlReply& reply, std::optional<LegacyPlayerLine> legacy, bool files_reply) noexcept
{
    if (legacy.has_value())
    {
        if (reply.ok)
        {
            queue_simple_ok(legacy->text);
        }
        else
        {
            queue_error(reply.error);
        }
    }
    else if (!reply.ok)
    {
        queue_reply_error(reply);
    }
    else if (files_reply || reply.files != nullptr)
    {
        if (reply.files != nullptr)
        {
            queue_files(*reply.files);
        }
        else
        {
            queue_json_error(refusal::files_page.message);
        }
    }
    else
    {
        begin_json(true);
        append_literal(",\"result\":{\"accepted\":true,\"apply_frame\":");
        append_uint(reply.apply_frame);
        append_literal(",\"events\":");
        append_uint(reply.events);
        append_literal("}}");
        finish_response();
    }
}

void queue_info() noexcept
{
    begin_json(true);
    append_literal(",\"info\":{\"kind\":\"waveform-player\",\"board\":\"" WFG_BOARD_NAME "\",\"firmware_version\":\"0.2.0\",\"sample_rate_hz\":48000,\"usb_serial\":\"");
    constexpr char hex[] = "0123456789ABCDEF";
    const std::uint32_t words[] = {WFG_SERIAL_WORD0, WFG_SERIAL_WORD1};

    for (const auto word : words)
        for (int i = 7; i >= 0; --i)
        {
            append_char(hex[(word >> (4 * i)) & 15]);
        }

    append_literal("\",\"build_id\":\"");
    append_text(firmware_build_id);
    append_literal("\",\"fw_commit\":\"");
    append_text(build_info::fw_commit);
    append_literal("\",\"fw_dirty\":");
    append_uint(std::to_underlying(build_info::fw_dirty));
    append_literal(R"(,"usb_pid":)");
    append_uint(usb_product_id);
    append_literal(R"(,"output":")");
    append_text(output_route_name(player_output_route()));
    append_literal(R"(","capabilities":["wav-files","live-controls","multi-cw","sample-indexed-replay","stepped-sweeps","reusable-playback","cut-injection","usb-audio","usb-diagnostics","playlist")");

    // A WFG_UAC_MONO_ALT image also offers the 16-bit mono alternate. Its PID,
    // commit equals the standard image's, so INFO says so.
    if constexpr (usb_audio_mono_offered)
    {
        append_literal(",\"usb-audio-mono\"");
    }

    // Likewise a WFG_SD_CLOCK_24MHZ image, whose SD bus runs at 24 MHz.
    if constexpr (sd_clock_24mhz)
    {
        append_literal(",\"sd-clock-24mhz\"");
    }

    const auto encoders = registered_encoders();

    if (!encoders.empty())
    {
        append_literal(",\"sd-wav-generation\"");
    }

    if (find_encoder("") != nullptr)
    {
        append_literal(",\"dd008-payload-upload\"");
    }

    // Every player image answers ENGINE? (the engine's state and reason). A
    // capability added later goes last, so each earlier one keeps its place.
    append_literal(
        ",\"engine-diagnostics\",\"watterson\",\"frequency-shift\",\"wideband-noise\",\"pulsed-cw\",\"swept-cw\",\"markov-cw\",\"fsk-cw\",\"faded-cw\",\"stream-id\",\"second-source\",\"files\"");
#if !defined(WFG_MSC_READ_ONLY)
    append_literal(",\"prepare\",\"prepare-play-experimental\"");
#endif
    append_literal("],\"encoders\":[");
    bool first = true;

    for (const auto& encoder : encoders)
    {
        if (!first)
        {
            append_literal(",");
        }

        first = false;
        append_literal("\"");
        append_literal(encoder.id);
        append_literal("\"");
    }

    append_literal("]");
#if defined(WFG_MSC_READ_ONLY)
    append_literal(",\"read_only\":true");
#endif
    append_literal(R"(,"request_capacity":)");
    append_uint(live_protocol::wire_capacity);
    append_literal(R"(,"response_capacity":)");
    append_uint(response_capacity);
    append_literal(R"(,"watterson_algorithm_version":)");
    append_uint(signal_lab::watterson_algorithm_version);
    append_literal(R"(,"frequency_shift_algorithm_version":)");
    append_uint(signal_lab::frequency_shift_algorithm_version);
    append_literal("}}");
    finish_response();
}

// The legacy player line that line spells, if it is one.
[[nodiscard]] std::optional<LegacyPlayerLine> find_legacy_player_line(std::string_view line) noexcept
{
    const auto found = std::ranges::find(legacy_player_lines, line, &LegacyPlayerLine::text);

    if (found == legacy_player_lines.end())
    {
        return std::nullopt;
    }

    return *found;
}

// WFG-LIVE/1 MEDIA HOST: BUSY during playback, upload or generation, or when the
// media layer can neither hand the card to the host nor start its initialization.
[[nodiscard]] m110::Status request_media_host() noexcept
{
    if (!player_allows_media_host() || WFG_MediaRequestHost() == kWFG_MediaHostRequestFailed)
    {
        return {.code = m110::StatusCode::busy, .message = "BUSY"};
    }

    return m110::Status::success();
}

// MEDIA LOCAL (both grammars). The media layer hands the card to the board only
// once the host has let go of its volume. A refusal changes nothing; its message
// is the reply code: HOST_WRITING (a WRITE(10) is between its blocks; ask again
// shortly) or HOST_LOCKED (the host prevents medium removal; dismount or eject
// the volume first), both busy, or BUSY (unavailable) when the card is in neither
// Host nor Local ownership.
[[nodiscard]] m110::Status request_media_local() noexcept
{
    const auto result = WFG_MediaRequestLocal();

    if (result == kWFG_MediaLocalHostWriting)
    {
        return {.code = m110::StatusCode::busy, .message = "HOST_WRITING"};
    }

    if (result == kWFG_MediaLocalHostLocked)
    {
        return {.code = m110::StatusCode::busy, .message = "HOST_LOCKED"};
    }

    if (result != kWFG_MediaLocalGranted)
    {
        return {.code = m110::StatusCode::unavailable, .message = "BUSY"};
    }

    return m110::Status::success();
}

// The read-only queries answered here; FILES? goes to the player task so only
// its existing FatFs owner reads the directory. Whether `kind` is queued.
[[nodiscard]] bool queue_query(live_protocol::Kind kind) noexcept
{
    using live_protocol::Kind;

    switch (kind)
    {
    case Kind::info:
        queue_info();
        return true;

    case Kind::status:
        queue_status("status");
        return true;

    case Kind::counters:
        queue_status("counters");
        return true;

    case Kind::usb_diagnostics:
        queue_usb_diagnostics();
        return true;

    case Kind::playlist_status:
        queue_playlist_status();
        return true;

    case Kind::engine_status:
        queue_engine_diagnostics();
        return true;

    case Kind::prepare_status:
        queue_prepare_diagnostics();
        return true;

    case Kind::prepare_play_status:
        queue_prepared_playback_diagnostics();
        return true;

    default:
        return false;
    }
}

// The legacy MEDIA LOCAL line: OK, ERR with the host's hold, or the original
// ERR MEDIA LOCAL FAILED when the card is in neither Host nor Local ownership.
void queue_legacy_media_local() noexcept
{
    const auto status = request_media_local();

    if (status.is_ok())
    {
        queue_simple_ok("MEDIA LOCAL");
    }
    else if (status.code == m110::StatusCode::busy)
    {
        queue_error(status.message);
    }
    else
    {
        queue_error("MEDIA LOCAL FAILED");
    }
}

// A request the USB task has taken: a WFG-LIVE/1 line's, or a legacy player
// line's (pending_legacy_line), whose sequence is 0. The USB task answers the
// queries and MEDIA itself; generation goes to the TX task, the rest to the
// player.
void handle_request(const live_protocol::Request& command) noexcept
{
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

        if (const m110::Status submitted = submit_tx_request(request); !submitted.is_ok())
        {
            queue_json_error(submitted.message);
        }
        else
        {
            tx_reply_waiting = true;
        }

        return;
    }

    if (queue_query(command.kind))
    {
        return;
    }

    if (command.kind == Kind::media_host || command.kind == Kind::media_local)
    {
        const auto status = command.kind == Kind::media_host ? request_media_host() : request_media_local();

        if (!status.is_ok())
        {
            queue_json_error(status.message);
            return;
        }

        begin_json(true);
        append_literal(",\"result\":{\"accepted\":true}}");
        finish_response();
        return;
    }

    if (const m110::Status submitted = submit_player_command(command); !submitted.is_ok())
    {
        pending_files_reply = false;

        if (pending_legacy_line.has_value())
        {
            queue_error(submitted.message);
        }
        else
        {
            queue_json_error(submitted.message);
        }

        return;
    }

    pending_files_reply = command.kind == Kind::files;
    player_reply_waiting = true;
}

void handle_control_line() noexcept
{
    std::string_view line = received_line();

    if (!line.empty() && line.back() == '\r')
    {
        line.remove_suffix(1);
    }

    // Preserve the original diagnostic tools. All new functionality uses sequences.
    if (line == "STATUS")
    {
        queue_status();
        return;
    }

    if (line == "MEDIA HOST" || line == "MEDIA LOCAL")
    {
        if (line == "MEDIA HOST")
        {
            if (!player_allows_media_host())
            {
                queue_error("BUSY");
            }
            else
            {
                const auto result = WFG_MediaRequestHost();

                if (result == kWFG_MediaHostRequestReady)
                {
                    queue_simple_ok("MEDIA HOST");
                }
                else if (result == kWFG_MediaHostRequestInitializing)
                {
                    queue_simple_ok("MEDIA HOST INIT");
                }
                else
                {
                    queue_error("MEDIA HOST FAILED");
                }
            }
        }
        else
        {
            queue_legacy_media_local();
        }

        return;
    }

    pending_legacy_line = find_legacy_player_line(line);

    if (pending_legacy_line.has_value())
    {
        live_protocol::Request legacy{};
        legacy.kind = pending_legacy_line->kind;
        handle_request(legacy);
        return;
    }

    const m110::Result<live_protocol::Request> parsed = live_protocol::parse(received_line(), last_sequence);

    // A refused line's reply echoes the sequence the line starts with, when
    // parse() could read one, and its error code is the refusal's message
    // (BAD_REQUEST).
    if (!parsed.has_value())
    {
        response_sequence = live_protocol::reply_sequence(received_line());
        queue_json_error(parsed.status().message);
        return;
    }

    handle_request(parsed.value());
}

void service_response() noexcept
{
    while (response_written < response_size)
    {
        const auto written = m110_tinyusb_cdc_write(response_buffer + response_written, static_cast<std::uint32_t>(response_size - response_written));

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

        if (player_reply_waiting)
        {
            discard_player_reply = true;
        }

        if (tx_usb_active() || tx_reply_waiting)
        {
            tx_usb_disconnect();
        }

        tx_reply_waiting = false;
    }

    connected_before = connected;

    if (tx_usb_active())
    {
        tx_usb_service();

        while (m110_tinyusb_cdc_available() && !tx_usb_blocked())
        {
            std::uint8_t byte{};

            if (m110_tinyusb_cdc_read(&byte, 1) != 1)
            {
                break;
            }

            tx_usb_feed(byte);
        }

        tx_usb_service();
        return;
    }

    if (tx_reply_waiting)
    {
        tx_protocol::Reply reply{};

        if (!take_tx_reply(reply))
        {
            return;
        }

        tx_reply_waiting = false;

        if (!reply.ok)
        {
            queue_json_error(reply.error);
        }
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

        if (!take_player_reply(reply))
        {
            return;
        }

        player_reply_waiting = false;
        const bool files_reply = pending_files_reply;
        pending_files_reply = false;

        if (discard_player_reply)
        {
            discard_player_reply = false;
            return;
        }

        queue_player_reply(reply, pending_legacy_line, files_reply);
    }

    if (response_size)
    {
        service_response();
        return;
    }

    while (m110_tinyusb_cdc_available() && !response_size && !player_reply_waiting && !tx_reply_waiting)
    {
        char byte{};

        if (m110_tinyusb_cdc_read(&byte, 1) != 1)
        {
            break;
        }

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

        if (byte == '\n')
        {
            handle_control_line();
            control_line_size = 0;
        }
        else if (control_line_size + 1 < control_line_capacity)
        {
            control_line()[control_line_size] = byte;
            ++control_line_size;
        }
        else
        {
            control_line_too_long = true;
            control_line_size = 0;
        }
    }

    if (response_size)
    {
        service_response();
    }
}

#endif

void usb_task_entry(void*) noexcept
{
    if (!status_of(m110_tinyusb_stack_initialize()).is_ok())
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

m110::Status start_usb_service() noexcept
{
    if (usb_task_handle != nullptr)
    {
        return {m110::StatusCode::invalid_argument, "USB diagnostic already started"};
    }

    // The hardware's own Status (USB_CLOCK_FAILED), so the caller has the reason.
    if (const m110::Status hardware = status_of(m110_usb_hardware_initialize()); !hardware.is_ok())
    {
        return hardware;
    }

    usb_task_handle = xTaskCreateStatic(&usb_task_entry, "wfg_usb", usb_stack_words, nullptr, 3U, usb_task_stack, &usb_task_control);

    if (usb_task_handle == nullptr)
    {
        return {m110::StatusCode::unavailable, "USB diagnostic task creation failed"};
    }

    return m110::Status::success();
}

} // namespace waveform_generator
