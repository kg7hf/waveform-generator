// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

#pragma once

// Engineering control protocol, deliberately separate from qualified WFG/1 ARM.
#include "common/cut_schedule.hpp"
#include "common/playlist.hpp"
#include "common/prepare_request.hpp"
#include "common/status.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace waveform_generator::live_protocol
{
inline constexpr char version[] = "WFG-LIVE/1";
// A request's bytes, its LF (or CR LF) included; INFO reports it as
// request_capacity. The longest request is a CUT of cut::max_entries entries.
inline constexpr std::size_t wire_capacity = 384U;
// That CUT at its widest: a 10-digit sequence, a 32-bit offset and a 4-digit
// length in every entry, then CR LF.
inline constexpr std::string_view widest_sequence{"4294967295 "};
inline constexpr std::string_view widest_cut_entry{"4294967295:4800:drop"};
inline constexpr std::string_view request_end{"\r\n"};
inline constexpr std::size_t widest_cut_request_bytes = widest_sequence.size() + cut::entries_prefix.size() + cut::max_entries * widest_cut_entry.size() + (cut::max_entries - 1U) + request_end.size();
static_assert(widest_cut_request_bytes <= wire_capacity, "one CUT request carries a whole schedule");
inline constexpr std::uint32_t cw_oscillator_capacity = 4U;
// output_codec and output_usb (OUTPUT CODEC, OUTPUT USB) select the player's
// output route; cut (CUT, CUT:<entries>) sets the next PLAY's cut schedule;
// playlist_start (PLAYLIST:<NAME>[:CONTINUE]) starts a playlist and
// playlist_ack (PLAYLIST ACK:<seq>) frees its result blocks. Live controls
// start at cw_on (live_command.hpp is_control()), so new configuration kinds
// go before it. The queries (INFO?, STATUS?, COUNTERS?, USB?, PLAYLIST?,
// ENGINE?, FILES? and PREPARE? come first; FILES? stores its optional offset in integer.
enum class Kind : std::uint8_t
{
    info,
    status,
    counters,
    usb_diagnostics,
    playlist_status,
    engine_status,
    files,
    prepare_status,
    prepare_play_status,
    load,
    play,
    stop,
    media_host,
    media_local,
    generate_file,
    output_codec,
    output_usb,
    cut,
    playlist_start,
    playlist_ack,
    seed,
    reference,
    prepare_file,
    prepare_play,
    cw_on,
    cw_frequency,
    cw_ci,
    static_on,
    static_rate,
    static_peak,
    fade,
    sweep_frequency,
    sweep_ci,
    sweep_fade
};
struct Request
{
    std::uint32_t seq{};
    Kind kind{};
    char name[13]{};
    char run_id[33]{};
    char input_name[13]{};
    char encoder[16]{};
    char profile[32]{};
    std::uint32_t rate{};
    std::uint32_t interleave{}; // 0 short, 1 long, 2 zero
    std::uint64_t integer{};
    bool scheduled{};
    std::uint64_t frame{};
    double value{};
    double end_value{};
    std::uint32_t steps{};
    std::uint32_t interval_ms{};
    std::uint32_t duration_ms{};
    std::uint32_t oscillator{};                // CW slot 0..3; legacy commands address slot 0.
    cut::CutList cuts{};                       // CUT: the entries as given, none for CUT alone; the player judges them
    playlist::Policy policy{};                 // PLAYLIST:<NAME>[:CONTINUE]: what an item error does; the name is in `name`
    std::uint32_t ack_seq{};                   // PLAYLIST ACK:<seq>: the newest result block the host has read
    prepare::Request preparation{};            // PREPARE request names and retention options
    std::uint32_t preparation_rate{};          // PREPAREPLAY measured end-to-end frames per second
    std::uint32_t preparation_margin_frames{}; // PREPAREPLAY conservative producer lead margin
};

// Why parse() refused a line, whatever refused it: on the wire every refusal
// is BAD_REQUEST, the reply's error code, so that is the message
// (rt1170/error_map.hpp publishes it).
inline constexpr m110::Status bad_request{.code = m110::StatusCode::invalid_argument, .message = "BAD_REQUEST"};

// The request a line holds, or bad_request. text excludes LF and may end in
// CR; no embedded controls or extra spaces. A positive canonical sequence
// above last_sequence becomes last_sequence even when the command after it is
// refused.
[[nodiscard]] m110::Result<Request> parse(std::string_view text, std::uint32_t& last_sequence) noexcept;
// The seq of the reply to a line, refused or not: the line's sequence when
// parse() could read one (the line fits wire_capacity, holds printable ASCII
// and starts with a canonical sequence of 1 to 2^32 - 1 and a space), else 0.
[[nodiscard]] std::uint32_t reply_sequence(std::string_view text) noexcept;
// A command after its sequence that starts with PLAYLIST, but PLAYLIST? (a
// plain query): PLAYLIST:<NAME>[:CONTINUE] or PLAYLIST ACK:<seq> into
// `request`, or why it is neither (playlist::bad_name, bad_policy, bad_ack,
// bad_command). parse() gives bad_request for every refusal.
[[nodiscard]] m110::Status parse_playlist_command(std::string_view text, Request& request) noexcept;
[[nodiscard]] bool filename(std::string_view text, std::string_view extension) noexcept;
// A canonical unsigned decimal of at most 64 bits, else bad_request.
[[nodiscard]] m110::Result<std::uint64_t> uint64(std::string_view text) noexcept;
} // namespace waveform_generator::live_protocol
