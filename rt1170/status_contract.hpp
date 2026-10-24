// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

#pragma once

#include "build_identity.hpp"
#include "common/card_listing.hpp"
#include "common/operation_timing.hpp"
#include "common/playlist.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <string_view>

namespace waveform_generator
{

inline constexpr std::size_t cdc_response_capacity = 4096U;
inline constexpr std::size_t max_uint32_decimal_chars = 10U;
inline constexpr std::size_t max_uint64_decimal_chars = 20U;
// "%.17g" of a finite positive double, e.g. 1.7976931348623157e+308 ("null" is shorter).
inline constexpr std::size_t max_json_double_chars = 23U;
// Longest strings STATUS? copies into the response, without their quotes.
// CMake caps WFG_BUILD_ID; the commit is a full commit name or the shorter unknown_commit.
inline constexpr std::size_t build_id_max_chars = 48U;
inline constexpr std::size_t fw_commit_max_chars = commit_hex_digits;
inline constexpr std::size_t run_id_max_chars = 32U;
inline constexpr std::size_t short_file_name_max_chars = 12U;
inline constexpr std::size_t sha256_hex_chars = 64U;
// The artifact error can be an encoder message; usb_service.cpp truncates it here.
inline constexpr std::size_t tx_error_max_chars = 64U;
// The characters of the engine reason (usb_service.cpp
// engine_diagnostics_reason(): player.hpp player_live_reason() when the live
// controls stopped the play, else player_engine_reason()) that ENGINE?
// carries; usb_service.cpp truncates it here. It is at least the longest message signal-lab and the
// error map give (design_doppler's refusal, 122 characters, is the longest):
// tests/test_engine_reason_budget.py checks every string literal of
// signal-lab's library and the error map against it, and
// tests/player_live_host_tests.cpp the engine's own.
inline constexpr std::size_t engine_reason_max_chars = 128U;

[[nodiscard]] constexpr std::size_t literal_length(const char* value) noexcept
{
    std::size_t length = 0U;

    while (value[length] != '\0')
    {
        ++length;
    }

    return length;
}

[[nodiscard]] constexpr std::size_t status_field_length(const char* name) noexcept
{
    return 1U + literal_length(name) + 1U + max_uint32_decimal_chars;
}

// Numeric fields of every STATUS form (text and JSON), in emission order.
inline constexpr const char* status_field_names[] = {
    "media_state",
    "media_error",
    "card_ready",
    "msc_ready",
    "msc_read_only",
    "block_count",
    "block_size",
    "init_attempts",
    "host_init_status",
    "host_detect_status",
    "card_init_status",
    "disk_init_status",
    "test_ready_calls",
    "not_ready_responses",
    "read_calls",
    "read_blocks",
    "write_calls",
    "write_blocks",
    "read_errors",
    "write_errors",
    "invalid_requests",
    "sync_cache_calls",
    "eject_requests",
    "ownership_handoffs",
    "cd_gpio3_level",
    "cd_cm7_gpio3_level",
    "cd_inserted",
    "player_state",
    "player_error",
    "data_bytes",
    "file_frames_total",
    "file_frames_enqueued",
    "ring_fill_frames",
    "ring_min_frames",
    "ring_min_pre_eof_frames",
    "ring_max_frames",
    "frames_requested",
    "file_frames_submitted",
    "pcm_drained",
    "eof_count",
    "eof_silence_frames",
    "sd_read_calls",
    "sd_bytes_read",
    "sd_short_reads",
    "sd_read_errors",
    "underruns",
    "first_underrun_frame",
    "max_sd_read_cycles",
    "audio_running",
    "audio_blocks",
    "audio_overruns",
    "audio_rx_errors",
    "audio_tx_errors",
    "audio_max_backlog_blocks",
    "audio_max_block_cycles",
    "audio_tx_races",
    "audio_tx_fifo_words_at_start",
    "audio_rx_fifo_words_at_start",
    "engine_state",
    "engine_error",
    "engine_stages",
    "engine_frames_in",
    "engine_frames_out",
    "engine_clipped",
    "engine_digest_hi",
    "engine_digest_lo",
    "engine_source_digest_hi",
    "engine_source_digest_lo",
    "engine_max_block_cycles",
    "engine_events_applied",
    "engine_events_dropped",
    "engine_arena_bytes",
    "ring_capacity_frames",
    "ring_high_watermark_frames",
    "ring_wake_watermark_frames",
    "ring_critical_frames",
    "ring_avg_frames",
    "ring_critical_events",
    "producer_rate_sps",
    "producer_worst_block_cycles",
    "producer_sleeps",
    "fw_commit_hi",
    "fw_commit_lo",
    "fw_dirty",
    // OutputRoute (rt1170/player.hpp): 0 codec, 1 usb.
    "output_route",
    // 1 while a host eject keeps the MSC medium absent until MEDIA HOST.
    "msc_ejected",
    // wfg_media_host_hold bits (platform/waveform_msc.h): 1 the host prevents
    // medium removal, 2 a host WRITE(10) is in progress. MEDIA LOCAL waits for 0.
    "msc_host_hold",
    // The USB sound card's capture stream (rt1170/usb_audio.hpp StreamCounters),
    // read when the reply is formed. Stream frame indices count frames since the
    // stream opened, its silent pre-fill included, and wrap after 24.8 h.
    "uac_stream_open",      // 1 while an alternate with a format is selected
    "uac_stream_opens",     // selections of an alternate with a format since boot
    "uac_stream_frames",    // frames sent since the stream opened, exact at the reply
    "uac_short_packets",    // since boot
    "uac_missed_intervals", // 1 ms intervals without a packet while streaming, since boot: a stream diagnostic
    "uac_host_volume",      // raw UAC2 volume in 1/256 dB as uint16 two's complement; reported, never applied
    "uac_host_mute",        // reported, never applied
    "usb_stack_asserts",    // TinyUSB TU_ASSERT failures since boot
    // The current or last play on the USB route (PlayStatistics), reset when
    // PLAY is accepted on either route (all zero for a codec play): the card
    // frames the frames confirmed sent cover (those frames themselves, unless
    // a CUT schedule inserted zeros or dropped frames: see USB?), the zlib
    // CRC-32 of each channel's sent samples as 3-byte little-endian PCM24, the
    // stream indices of the first and last frame sent (0 until one is), the
    // extended USB frame number in which the
    // first went out, the stream index at which PLAY was accepted, the 1 ms
    // intervals missed while the play had file frames queued, armed or not
    // (each gap counted once a completion ends it), and the stream index of
    // the first frame sent after its first miss (0 when none).
    "uac_play_file_frames",
    "uac_play_crc32_l",
    "uac_play_crc32_r",
    "uac_play_first_frame_index",
    "uac_play_first_frame_sof",
    "uac_play_last_frame_index",
    "uac_play_accept_frame_index",
    "uac_play_missed_intervals",
    "uac_play_first_miss_frame_index",
};

[[nodiscard]] constexpr std::size_t status_response_worst_case_bytes() noexcept
{
    std::size_t total = literal_length("OK STATUS");

    for (const char* field : status_field_names)
    {
        total += status_field_length(field);
    }

    return total + 1U + 1U;
}

static_assert(status_response_worst_case_bytes() <= cdc_response_capacity);

// A JSON member ,"name":value at its widest. A string value counts its quotes.
struct StatusJsonField
{
    const char* name;
    std::size_t max_value_chars;
};

[[nodiscard]] constexpr std::size_t json_string_chars(std::size_t max_chars) noexcept
{
    return 1U + max_chars + 1U;
}

[[nodiscard]] constexpr std::size_t json_member_length(const char* name, std::size_t max_value_chars) noexcept
{
    return 2U + literal_length(name) + 2U + max_value_chars;
}

// Members STATUS?/COUNTERS? add after status_field_names, in emission order.
inline constexpr auto status_json_fields = std::to_array<StatusJsonField>({
    {.name = "live_seed", .max_value_chars = max_uint64_decimal_chars},
    {.name = "live_frame", .max_value_chars = max_uint64_decimal_chars},
    {.name = "live_digest_hi", .max_value_chars = max_uint32_decimal_chars},
    {.name = "live_digest_lo", .max_value_chars = max_uint32_decimal_chars},
    {.name = "live_clipped", .max_value_chars = max_uint64_decimal_chars},
    {.name = "live_events", .max_value_chars = max_uint32_decimal_chars},
    {.name = "live_queue_free", .max_value_chars = max_uint32_decimal_chars},
    {.name = "live_capture_free", .max_value_chars = max_uint32_decimal_chars},
    {.name = "tx_artifact_state", .max_value_chars = max_uint32_decimal_chars},
    {.name = "tx_payload_bytes", .max_value_chars = max_uint32_decimal_chars},
    {.name = "tx_frames_written", .max_value_chars = max_uint64_decimal_chars},
    {.name = "tx_total_frames", .max_value_chars = max_uint64_decimal_chars},
    {.name = "tx_filename", .max_value_chars = json_string_chars(short_file_name_max_chars)},
    {.name = "tx_wav_sha256", .max_value_chars = json_string_chars(sha256_hex_chars)},
    {.name = "tx_error", .max_value_chars = json_string_chars(tx_error_max_chars)},
    {.name = "live_reference_rms", .max_value_chars = max_json_double_chars},
    {.name = "selected_file", .max_value_chars = json_string_chars(short_file_name_max_chars)},
    {.name = "build_id", .max_value_chars = json_string_chars(build_id_max_chars)},
    {.name = "fw_commit", .max_value_chars = json_string_chars(fw_commit_max_chars)},
});

// usb_service.cpp's begin_json() prefix at its widest: a quoted run id is
// longer than null.
[[nodiscard]] constexpr std::size_t json_reply_prefix_bytes() noexcept
{
    return literal_length(R"({"seq":)") + max_uint32_decimal_chars + literal_length(R"(,"protocol":"WFG-LIVE/1","ok":true,"state":)") + max_uint32_decimal_chars + literal_length(R"(,"run_id":)") +
           json_string_chars(run_id_max_chars);
}

// The "}}" that closes a reply's object and the reply, then the newline and
// the terminator.
[[nodiscard]] constexpr std::size_t json_reply_end_bytes() noexcept
{
    return literal_length("}}") + 1U + 1U;
}

// The complete STATUS?/COUNTERS? reply as usb_service.cpp builds it: the
// begin_json() prefix, the counters object, and the newline and terminator.
[[nodiscard]] constexpr std::size_t status_json_worst_case_bytes() noexcept
{
    // COUNTERS? names the object with the longer key.
    std::size_t total = json_reply_prefix_bytes() + literal_length(R"(,"counters":{"format":"engineering-counters")");

    for (const char* field : status_field_names)
    {
        total += json_member_length(field, max_uint32_decimal_chars);
    }

    for (const auto& field : status_json_fields)
    {
        total += json_member_length(field.name, field.max_value_chars);
    }

    return total + json_reply_end_bytes();
}

static_assert(status_json_worst_case_bytes() <= cdc_response_capacity);

// The USB? reply (WFG-LIVE/1 only): the begin_json() prefix, then this object,
// whose members after "format" are usb_field_names, each a uint32. STATUS keeps
// its own fields; new USB diagnostics go here, never into STATUS, whose JSON
// form is within a few bytes of the response buffer.
inline constexpr std::string_view usb_json_object_header{R"(,"usb":{"format":"usb-diagnostics")"};

// Members of the USB? "usb" object after its format, in emission order: the
// capture stream's diagnostics as STATUS also reports them, then the ones only
// USB? carries (rt1170/usb_audio.hpp StreamCounters and StackAsserts, the
// MSC transfer maxima and SD bus clock of platform/waveform_msc.h, and the
// play's cuts).
inline constexpr auto usb_field_names = std::to_array<const char*>({
    "uac_stream_open",
    "uac_stream_opens",
    "uac_stream_frames",
    "uac_short_packets",
    "uac_missed_intervals",
    "uac_host_volume",
    "uac_host_mute",
    "usb_stack_asserts",
    "uac_rejected_chunks", // chunks the IN FIFO refused or that found no free record (frames lost), since boot
    // Miss attribution. The location and the largest gap restart when the
    // stream opens (stream frame indices); the stream counts run since boot;
    // the play's restarts with every accepted PLAY, like STATUS's uac_play_*.
    "uac_last_miss_frame_index",   // stream index of the first frame sent after the latest gap that still counts a miss; 0 when none
    "uac_last_miss_unprimed",      // how many of that gap's missed intervals are unprimed (device late)
    "uac_last_miss_play",          // 1 when that gap delayed the file frames of the USB play then current
    "uac_max_gap_frames",          // the largest gap between IN completions in USB frames, as the USB ISR read them
    "uac_watchdog_expiries",       // service watchdog samples while streaming: one per millisecond of a host pause, and per held-off USB ISR
    "uac_unprimed_intervals",      // the part of uac_missed_intervals counted unprimed (device late)
    "uac_play_unprimed_intervals", // the part of uac_play_missed_intervals counted unprimed (device late)
    // TinyUSB's assertion failures since boot, failed TU_ASSERT checks and its
    // direct TU_BREAKPOINT() calls (rt1170/usb_audio.hpp StackAsserts), all 0
    // until the first. A pc is a code address inside the call to the assert
    // hook in the TinyUSB function that failed, for arm-none-eabi-addr2line
    // against the image's ELF.
    "usb_assert_count",            // failures since boot, read with the rest of the record (equals usb_stack_asserts)
    "usb_first_assert_pc",         // where the first failed
    "usb_last_assert_pc",          // where the latest failed
    "usb_last_assert_frame_index", // uac_stream_frames when the latest failed: frames sent since the stream opened
    "usb_last_assert_tick",        // the RTOS tick (ms since the scheduler started) when the latest failed
    // The capture format (rt1170/usb_audio.hpp StreamFormat): the open stream's,
    // all 0 while closed, then the one the current or last USB play's first
    // file frame went out in, 0 until it has gone out.
    "uac_stream_alt",           // the streaming interface's alternate setting: 1 24-bit stereo, 2 16-bit stereo, 3 16-bit mono
    "uac_stream_bits",          // valid bits per sample on the wire: 24 or 16
    "uac_stream_channels",      // channels on the wire: 2, or 1 (the left channel)
    "uac_play_stream_bits",     // the play's bits per sample on the wire
    "uac_play_stream_channels", // the play's channels on the wire
    // The microSD card's MSC transfers since boot (platform/waveform_msc.h):
    // TinyUSB hands a READ(10) or WRITE(10) over in chunks of up to 16 KB (32
    // blocks), and each chunk is one SD command. All 0 until the first.
    "msc_max_write_us",        // the longest SD write of one chunk, passed or failed, in microseconds
    "msc_max_read_us",         // the longest SD read of one chunk, likewise
    "msc_max_blocks_per_call", // the most 512-byte blocks one chunk moved in its SD command
    // The bus those transfers run on (platform/waveform_msc.h): the SD bus
    // clock in Hz as card initialisation left it, 0 while no card is ready.
    // 49500000 is High Speed from the 198 MHz root; 24750000 a card that
    // stayed in Default Speed; 24000000 a WFG_SD_CLOCK_24MHZ image.
    "sd_bus_clock_hz",
    // The current or last play's CUT injector (rt1170/player.hpp
    // PlayCutStatistics), on either route, restarted by every accepted PLAY:
    // the cuts that took effect within the frames the route confirmed sent
    // (an exact prefix after STOP, a close or a stall). Apart from the miss
    // counters: a cut never counts as a missed interval.
    "uac_play_cut_count",       // cuts that took effect
    "uac_play_cut_drop_frames", // engine output frames their DROPs discarded (the engine processed them; none reached the output)
    "uac_play_cut_zero_frames", // zero frames their ZEROs sent
    "uac_play_cut_first_index", // stream index where the first took effect (a codec play: frames from its first); 0 while none has
    "uac_play_cut_rejected",    // entries at or past the end of the engine's output, ignored; counted once the whole stream was sent (0 for a play that ended part way)
    "uac_play_sent_frames",     // the play's frames sent, after its cuts: last - first + 1 of a USB play's frame indices
    // The current or last play's engine deadline misses (player.hpp
    // player_deadline_misses()), on either route: blocks of the engine's
    // output ready only once the running output had drained the FIFO below
    // ring_critical_frames. A looped PLAYLIST item ends at the first.
    "engine_deadline_misses",
    // The USB interrupt's EP0 order since boot (rt1170/usb_ep0_order.hpp,
    // #32): EP0 completions it passed on ahead of a SETUP they preceded on
    // the bus, which TinyUSB would otherwise have matched against that
    // SETUP's request; and SETUPs it could not keep behind such a
    // completion: let go with one still pending (at the pass bound, or for
    // another event), or not held because the hold was full (0 unless the
    // controller never stops completing).
    "usb_ep0_reordered",
    "usb_ep0_unordered",
    // IN completions since boot whose reported size was not the packet
    // TinyUSB loaded for them (rt1170/usb_audio.hpp StreamCounters, #45):
    // each still counts as sent, since TinyUSB took the packet from the FIFO,
    // and as a short packet. 0 unless the controller reports a transfer that
    // ended in error; it stays at 65535 once there.
    "uac_sent_mismatches",
});

// The complete USB? reply as usb_service.cpp builds it.
[[nodiscard]] constexpr std::size_t usb_json_worst_case_bytes() noexcept
{
    std::size_t total = json_reply_prefix_bytes() + usb_json_object_header.size();

    for (const char* field : usb_field_names)
    {
        total += json_member_length(field, max_uint32_decimal_chars);
    }

    return total + json_reply_end_bytes();
}

static_assert(usb_json_worst_case_bytes() <= cdc_response_capacity);

// The FILES? page response: a 24-entry FatFs-order page with uint64 offsets
// and sizes. A UTF-8 alias or filename is at most name_capacity - 1 bytes; each byte can
// at most double when a quote or backslash is JSON escaped.
inline constexpr std::size_t files_name_escaped_max_bytes = std::size_t{2U} * (card_listing::name_capacity - 1U);
static_assert(card_listing::page_capacity == 24U && card_listing::name_capacity == 37U, "FILES? wire budget assumes 24 entries and at most 36 UTF-8 bytes per name");

[[nodiscard]] constexpr std::size_t files_entry_worst_case_bytes() noexcept
{
    return literal_length(R"({"name":")") + files_name_escaped_max_bytes + literal_length(R"(","size_bytes":)") + max_uint64_decimal_chars + literal_length("}");
}

[[nodiscard]] constexpr std::size_t files_json_worst_case_bytes() noexcept
{
    return json_reply_prefix_bytes() + literal_length(R"(,"files":{"offset":)") + max_uint64_decimal_chars + literal_length(R"(,"total":)") + max_uint64_decimal_chars +
           literal_length(R"(,"next_offset":)") + std::max(max_uint64_decimal_chars, literal_length("null")) + literal_length(R"(,"entries":[)") +
           card_listing::page_capacity * files_entry_worst_case_bytes() + (card_listing::page_capacity - 1U) * literal_length(",") + literal_length("]") + json_reply_end_bytes();
}

static_assert(files_json_worst_case_bytes() <= cdc_response_capacity, "FILES? serializes a full 24-entry page within the CDC response buffer");

// The decimal digits of `value`.
[[nodiscard]] constexpr std::size_t decimal_chars(std::uint64_t value) noexcept
{
    constexpr std::uint64_t radix = 10U;
    std::size_t digits = 1U;

    for (; value >= radix; value /= radix)
    {
        ++digits;
    }

    return digits;
}

static_assert(decimal_chars(0U) == 1U && decimal_chars(UINT32_MAX) == max_uint32_decimal_chars);

// The longest of `words`.
[[nodiscard]] constexpr std::size_t longest_word(std::span<const std::string_view> words) noexcept
{
    std::size_t longest = 0U;

    for (const auto word : words)
    {
        longest = std::max(longest, word.size());
    }

    return longest;
}

[[nodiscard]] constexpr std::size_t longest_word(std::initializer_list<std::string_view> words) noexcept
{
    return longest_word(std::span<const std::string_view>{words.begin(), words.size()});
}

// A JSON value that is a string of at most `max_chars` characters, or null.
[[nodiscard]] constexpr std::size_t json_nullable_string_chars(std::size_t max_chars) noexcept
{
    return std::max(json_string_chars(max_chars), literal_length("null"));
}

// The ENGINE? reply (WFG-LIVE/1 only): the begin_json() prefix, then this
// object, whose members after "format" are engine_json_fields: STATUS's
// engine_state and engine_error, and the reason, a JSON string of at most
// engine_reason_max_chars characters (every one a quote or a backslash
// escaped, at worst), or null while there is none. It has its own budget, as
// USB? does; STATUS stays as it is.
inline constexpr std::string_view engine_json_object_header{R"(,"engine":{"format":"engine-diagnostics")"};
inline constexpr std::size_t engine_reason_escaped_chars = 2U * engine_reason_max_chars;
inline constexpr auto engine_json_fields = std::to_array<StatusJsonField>({
    {.name = "watterson_version", .max_value_chars = max_uint32_decimal_chars},
    {.name = "watterson_digest_hi", .max_value_chars = max_uint32_decimal_chars},
    {.name = "watterson_digest_lo", .max_value_chars = max_uint32_decimal_chars},
    {.name = "watterson_latency_frames", .max_value_chars = max_uint32_decimal_chars},
    {.name = "watterson_paths", .max_value_chars = max_uint32_decimal_chars},
    {.name = "second_source_version", .max_value_chars = max_uint32_decimal_chars},
    {.name = "second_source_source_frames", .max_value_chars = max_uint32_decimal_chars},
    {.name = "second_source_excerpt_start_frame", .max_value_chars = max_uint32_decimal_chars},
    {.name = "second_source_excerpt_frames", .max_value_chars = max_uint32_decimal_chars},
    {.name = "second_source_digest_hi", .max_value_chars = max_uint32_decimal_chars},
    {.name = "second_source_digest_lo", .max_value_chars = max_uint32_decimal_chars},
    {.name = "second_source_latency_frames", .max_value_chars = max_uint32_decimal_chars},
    {.name = "second_source_reference_rms", .max_value_chars = max_json_double_chars},
    {.name = "engine_state", .max_value_chars = max_uint32_decimal_chars},
    {.name = "engine_error", .max_value_chars = max_uint32_decimal_chars},
    {.name = "engine_reason", .max_value_chars = std::max(json_string_chars(engine_reason_escaped_chars), literal_length("null"))},
});

// The complete ENGINE? reply as usb_service.cpp builds it.
[[nodiscard]] constexpr std::size_t engine_json_worst_case_bytes() noexcept
{
    std::size_t total = json_reply_prefix_bytes() + engine_json_object_header.size();

    for (const auto& field : engine_json_fields)
    {
        total += json_member_length(field.name, field.max_value_chars);
    }

    return total + json_reply_end_bytes();
}

static_assert(engine_json_worst_case_bytes() <= cdc_response_capacity, "ENGINE? carries the whole reason, escaped, within the response buffer");

inline constexpr std::size_t prepare_reason_max_chars = 128U;
inline constexpr auto prepare_json_fields = std::to_array<StatusJsonField>({
    {.name = "phase", .max_value_chars = max_uint32_decimal_chars},
    {.name = "error", .max_value_chars = json_string_chars(6U * prepare_reason_max_chars)},
    {.name = "source", .max_value_chars = json_string_chars(short_file_name_max_chars)},
    {.name = "scenario", .max_value_chars = json_string_chars(short_file_name_max_chars)},
    {.name = "output", .max_value_chars = json_string_chars(short_file_name_max_chars)},
    {.name = "retention", .max_value_chars = json_string_chars(9U)},
    {.name = "seed", .max_value_chars = max_uint64_decimal_chars},
    {.name = "input_frames", .max_value_chars = max_uint64_decimal_chars},
    {.name = "output_frames", .max_value_chars = max_uint64_decimal_chars},
    {.name = "total_input_frames", .max_value_chars = max_uint64_decimal_chars},
    {.name = "wav_sha256", .max_value_chars = json_string_chars(sha256_hex_chars)},
    {.name = "pcm_sha256", .max_value_chars = json_string_chars(sha256_hex_chars)},
});

[[nodiscard]] constexpr std::size_t prepare_json_worst_case_bytes() noexcept
{
    std::size_t total = json_reply_prefix_bytes() + literal_length(R"(,"prepare":{)");

    for (const auto& field : prepare_json_fields)
    {
        total += json_member_length(field.name, field.max_value_chars);
    }

    // The first member follows '{' directly; json_member_length includes
    // the comma used by every subsequent member.
    return total - 1U + json_reply_end_bytes();
}

static_assert(prepare_json_worst_case_bytes() <= cdc_response_capacity, "PREPARE? fits with escaped failure text and 64-bit counts");

inline constexpr std::array<const char*, operation_timing::phase_count> prepared_timing_phase_names{"setup", "source_read", "render", "append", "commit", "read", "finalize", "cleanup"};

[[nodiscard]] constexpr std::size_t prepared_timing_stats_json_chars() noexcept
{
    std::size_t total{};
    total += json_member_length("calls", max_uint64_decimal_chars);
    total += json_member_length("total_cycles", max_uint64_decimal_chars);
    total += json_member_length("max_cycles", max_uint32_decimal_chars);
    total += json_member_length("rejected", max_uint64_decimal_chars);
    total += json_member_length("saturated", 5U);
    return total + 1U;
}

[[nodiscard]] constexpr std::size_t prepared_timing_phases_json_chars() noexcept
{
    std::size_t total{};

    for (const char* name : prepared_timing_phase_names)
    {
        total += json_member_length(name, prepared_timing_stats_json_chars());
    }

    return total + 1U;
}

[[nodiscard]] constexpr std::size_t prepared_timing_json_chars() noexcept
{
    return json_member_length("cycle_hz", max_uint32_decimal_chars) + json_member_length("tick_hz", max_uint32_decimal_chars) + json_member_length("phases", prepared_timing_phases_json_chars()) + 1U;
}

inline constexpr auto prepared_playback_json_fields = std::to_array<StatusJsonField>({
    {.name = "state", .max_value_chars = max_uint32_decimal_chars},
    {.name = "operation_id", .max_value_chars = max_uint64_decimal_chars},
    {.name = "rate", .max_value_chars = max_uint32_decimal_chars},
    {.name = "margin_frames", .max_value_chars = max_uint32_decimal_chars},
    {.name = "total_frames", .max_value_chars = max_uint64_decimal_chars},
    {.name = "required_lead_frames", .max_value_chars = max_uint64_decimal_chars},
    {.name = "produced_frames", .max_value_chars = max_uint64_decimal_chars},
    {.name = "written_frames", .max_value_chars = max_uint64_decimal_chars},
    {.name = "committed_frames", .max_value_chars = max_uint64_decimal_chars},
    {.name = "queued_frames", .max_value_chars = max_uint64_decimal_chars},
    {.name = "consumed_frames", .max_value_chars = max_uint64_decimal_chars},
    {.name = "status", .max_value_chars = max_uint32_decimal_chars},
    {.name = "timing", .max_value_chars = prepared_timing_json_chars()},
    {.name = "active", .max_value_chars = 5U},
    {.name = "started", .max_value_chars = 5U},
    {.name = "artifact_finalized", .max_value_chars = 5U},
    {.name = "action_pending", .max_value_chars = 5U},
    {.name = "owner_released", .max_value_chars = 5U},
    {.name = "reason", .max_value_chars = json_string_chars(6U * prepare_reason_max_chars)},
});

[[nodiscard]] constexpr std::size_t prepared_playback_json_worst_case_bytes() noexcept
{
    std::size_t total = json_reply_prefix_bytes() + literal_length(R"(,"prepare_play":{)");

    for (const auto& field : prepared_playback_json_fields)
    {
        total += json_member_length(field.name, field.max_value_chars);
    }

    return total - 1U + json_reply_end_bytes();
}

static_assert(prepared_playback_json_worst_case_bytes() <= cdc_response_capacity, "PREPAREPLAY? fits with escaped reason, timing phases and 64-bit watermarks");

// The PLAYLIST? reply (WFG-LIVE/1 only): the begin_json() prefix, then the
// playlist object: its format and the progress members
// (playlist_progress_fields), then "blocks", an array of at most
// playlist::blocks_per_reply result blocks, the oldest first, each an object
// of playlist_block_fields (common/playlist.hpp ResultBlock). It has its own
// budget, as USB? does; STATUS stays as it is.
inline constexpr std::string_view playlist_json_object_header{R"(,"playlist":{"format":"playlist")"};
inline constexpr std::string_view playlist_blocks_open{R"(,"blocks":[)"};
inline constexpr std::string_view playlist_blocks_close{"]"};
inline constexpr std::size_t playlist_state_chars = longest_word(
    {playlist::state_name(playlist::State::idle), playlist::state_name(playlist::State::running), playlist::state_name(playlist::State::gap), playlist::state_name(playlist::State::paused_ring_full),
     playlist::state_name(playlist::State::done), playlist::state_name(playlist::State::aborted), playlist::state_name(playlist::State::error)});
inline constexpr std::size_t playlist_policy_chars = longest_word({playlist::policy_name(playlist::Policy::stop), playlist::policy_name(playlist::Policy::continue_on_error)});
inline constexpr std::size_t playlist_route_chars = longest_word({playlist::route_name(playlist::Route::codec), playlist::route_name(playlist::Route::usb)});
inline constexpr std::size_t playlist_outcome_chars =
    longest_word({playlist::outcome_name(playlist::Outcome::done), playlist::outcome_name(playlist::Outcome::aborted), playlist::outcome_name(playlist::Outcome::error)});
inline constexpr std::size_t playlist_end_cause_chars = longest_word(playlist::end_cause_names);

// Members of the PLAYLIST? "playlist" object after its format, in emission order.
inline constexpr auto playlist_progress_fields = std::to_array<StatusJsonField>({
    {.name = "state", .max_value_chars = json_string_chars(playlist_state_chars)},
    {.name = "name", .max_value_chars = json_string_chars(playlist::name_max_chars)},
    {.name = "run", .max_value_chars = max_uint32_decimal_chars},
    {.name = "policy", .max_value_chars = json_string_chars(playlist_policy_chars)},
    {.name = "items", .max_value_chars = decimal_chars(playlist::max_items)},
    {.name = "index", .max_value_chars = decimal_chars(playlist::max_items)},
    {.name = "completed", .max_value_chars = decimal_chars(playlist::max_items)},
    {.name = "failed", .max_value_chars = decimal_chars(playlist::max_items)},
    {.name = "pending", .max_value_chars = decimal_chars(playlist::result_capacity)},
    {.name = "oldest_seq", .max_value_chars = max_uint32_decimal_chars},
    {.name = "next_seq", .max_value_chars = max_uint32_decimal_chars},
    // The RTOS tick when the reply was read (playlist::Snapshot::tick).
    {.name = "tick", .max_value_chars = max_uint32_decimal_chars},
});

// Members of one result block, in emission order: which item, how it ended
// (player_error is STATUS's), its per-play statistics under the names STATUS
// and USB? report them by, then its gap, scheduled start and ticks, then its
// attenuation (att=, in tenths of a dB below full level), its duration
// (dur=, in frames, null without it), the loops it completed, why it ended
// and its engine deadline misses (USB?'s name).
inline constexpr auto playlist_block_fields = std::to_array<StatusJsonField>({
    {.name = "seq", .max_value_chars = max_uint32_decimal_chars},
    {.name = "run", .max_value_chars = max_uint32_decimal_chars},
    {.name = "item", .max_value_chars = decimal_chars(playlist::max_items - 1U)},
    {.name = "label", .max_value_chars = json_nullable_string_chars(playlist::label_max_chars)},
    {.name = "file", .max_value_chars = json_string_chars(playlist::name_max_chars)},
    {.name = "route", .max_value_chars = json_string_chars(playlist_route_chars)},
    {.name = "result", .max_value_chars = json_string_chars(playlist_outcome_chars)},
    {.name = "player_error", .max_value_chars = max_uint32_decimal_chars},
    {.name = "file_frames_total", .max_value_chars = max_uint32_decimal_chars},
    {.name = "scn", .max_value_chars = json_nullable_string_chars(playlist::name_max_chars)},
    {.name = "seed", .max_value_chars = max_uint32_decimal_chars},
    {.name = "scn_crc32", .max_value_chars = max_uint32_decimal_chars},
    {.name = "engine_state", .max_value_chars = max_uint32_decimal_chars},
    {.name = "engine_error", .max_value_chars = max_uint32_decimal_chars},
    {.name = "uac_play_file_frames", .max_value_chars = max_uint32_decimal_chars},
    {.name = "uac_play_sent_frames", .max_value_chars = max_uint32_decimal_chars},
    {.name = "uac_play_crc32_l", .max_value_chars = max_uint32_decimal_chars},
    {.name = "uac_play_crc32_r", .max_value_chars = max_uint32_decimal_chars},
    {.name = "uac_play_first_frame_index", .max_value_chars = max_uint32_decimal_chars},
    {.name = "uac_play_last_frame_index", .max_value_chars = max_uint32_decimal_chars},
    {.name = "uac_play_first_frame_sof", .max_value_chars = max_uint32_decimal_chars},
    {.name = "uac_play_missed_intervals", .max_value_chars = max_uint32_decimal_chars},
    {.name = "uac_play_first_miss_frame_index", .max_value_chars = max_uint32_decimal_chars},
    {.name = "uac_play_stream_bits", .max_value_chars = max_uint32_decimal_chars},
    {.name = "uac_play_stream_channels", .max_value_chars = max_uint32_decimal_chars},
    {.name = "uac_play_cut_count", .max_value_chars = max_uint32_decimal_chars},
    {.name = "uac_play_cut_drop_frames", .max_value_chars = max_uint32_decimal_chars},
    {.name = "uac_play_cut_zero_frames", .max_value_chars = max_uint32_decimal_chars},
    {.name = "uac_play_cut_first_index", .max_value_chars = max_uint32_decimal_chars},
    {.name = "uac_play_cut_rejected", .max_value_chars = max_uint32_decimal_chars},
    {.name = "underruns", .max_value_chars = max_uint32_decimal_chars},
    {.name = "pcm_drained", .max_value_chars = max_uint32_decimal_chars},
    {.name = "gap_frames", .max_value_chars = decimal_chars(playlist::max_gap.value)},
    {.name = "scheduled_index", .max_value_chars = max_uint32_decimal_chars},
    {.name = "start_late", .max_value_chars = literal_length("1")},
    {.name = "start_tick", .max_value_chars = max_uint32_decimal_chars},
    {.name = "end_tick", .max_value_chars = max_uint32_decimal_chars},
    {.name = "att_tenths_db", .max_value_chars = decimal_chars(attenuation::max_tenths)},
    {.name = "dur_frames", .max_value_chars = std::max(decimal_chars(playlist::max_duration.value), literal_length("null"))},
    {.name = "loops", .max_value_chars = max_uint32_decimal_chars},
    {.name = "end_cause", .max_value_chars = json_string_chars(playlist_end_cause_chars)},
    {.name = "engine_deadline_misses", .max_value_chars = max_uint32_decimal_chars},
    {.name = "watterson_version", .max_value_chars = max_uint32_decimal_chars},
    {.name = "watterson_digest_hi", .max_value_chars = max_uint32_decimal_chars},
    {.name = "watterson_digest_lo", .max_value_chars = max_uint32_decimal_chars},
    {.name = "watterson_latency_frames", .max_value_chars = max_uint32_decimal_chars},
    {.name = "watterson_paths", .max_value_chars = max_uint32_decimal_chars},
    {.name = "second_source_version", .max_value_chars = max_uint32_decimal_chars},
    {.name = "second_source_source_frames", .max_value_chars = max_uint32_decimal_chars},
    {.name = "second_source_excerpt_start_frame", .max_value_chars = max_uint32_decimal_chars},
    {.name = "second_source_excerpt_frames", .max_value_chars = max_uint32_decimal_chars},
    {.name = "second_source_digest_hi", .max_value_chars = max_uint32_decimal_chars},
    {.name = "second_source_digest_lo", .max_value_chars = max_uint32_decimal_chars},
    {.name = "second_source_latency_frames", .max_value_chars = max_uint32_decimal_chars},
    {.name = "second_source_reference_rms", .max_value_chars = max_json_double_chars},
    {.name = "engine_digest_hi", .max_value_chars = max_uint32_decimal_chars},
    {.name = "engine_digest_lo", .max_value_chars = max_uint32_decimal_chars},
});

// One result block at its widest: its members in braces, the first without
// its comma.
[[nodiscard]] constexpr std::size_t playlist_block_worst_case_bytes() noexcept
{
    std::size_t total = literal_length("{}") - literal_length(",");

    for (const auto& field : playlist_block_fields)
    {
        total += json_member_length(field.name, field.max_value_chars);
    }

    return total;
}

// The complete PLAYLIST? reply as usb_service.cpp builds it, with
// playlist::blocks_per_reply blocks and the commas between them.
[[nodiscard]] constexpr std::size_t playlist_json_worst_case_bytes() noexcept
{
    std::size_t total = json_reply_prefix_bytes() + playlist_json_object_header.size();

    for (const auto& field : playlist_progress_fields)
    {
        total += json_member_length(field.name, field.max_value_chars);
    }

    total += playlist_blocks_open.size() + playlist::blocks_per_reply * playlist_block_worst_case_bytes() + (playlist::blocks_per_reply - 1U) * literal_length(",") + playlist_blocks_close.size();
    return total + json_reply_end_bytes();
}

static_assert(playlist_json_worst_case_bytes() <= cdc_response_capacity, "PLAYLIST? carries playlist::blocks_per_reply blocks within the response buffer");
static_assert(playlist_json_worst_case_bytes() + literal_length(",") + playlist_block_worst_case_bytes() > cdc_response_capacity, "PLAYLIST? carries as many blocks as fit");

} // namespace waveform_generator
