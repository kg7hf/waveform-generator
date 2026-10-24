// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

#pragma once

#include "common/card_listing.hpp"
#include "common/cut_schedule.hpp"
#include "common/finite_playback.hpp"
#include "common/live_protocol.hpp"
#include "common/operation_timing.hpp"
#include "common/playlist.hpp"
#include "common/prepare_request.hpp"
#include "common/status.hpp"
#include "usb_audio.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>

namespace waveform_generator
{

// Where PLAY sends its audio (OUTPUT CODEC / OUTPUT USB). The numbers are the
// STATUS output_route wire values: never renumber or reuse one.
enum class OutputRoute : std::uint8_t
{
    codec = 0U, // WM8960 line output; the route at boot
    usb = 1U,   // the UAC2 capture stream of the composite USB device
};

// The INFO "output" name of a route.
[[nodiscard]] constexpr std::string_view output_route_name(OutputRoute route) noexcept
{
    return route == OutputRoute::usb ? "usb" : "codec";
}

enum class PlayerState : std::uint32_t
{
    stopped = 0U,
    waiting_for_command = 1U,
    mounting = 2U,
    opening = 3U,
    buffering = 4U,
    playing = 5U,
    draining = 6U,
    done = 7U,
    aborted = 8U,
    fault = 0xFFU,
};

enum class PlayerError : std::uint32_t
{
    none = 0U,
    host_init = 1U,
    card_detect = 2U,
    mount = 3U,
    open = 4U,
    header_io = 5U,
    invalid_riff = 6U,
    missing_format = 7U,
    missing_data = 8U,
    unsupported_format = 9U,
    seek = 10U,
    sd_read = 11U,
    codec_config = 12U,
    codec_level = 13U,
    codec_start = 14U,
    underrun = 15U,
    impairment = 16U,
    native_source = 17U,
    live_control = 18U,
    // The USB route's stream closed (alternate 0, bus reset, unplug,
    // SET_CONFIGURATION) after PLAY was accepted and before every file frame
    // was confirmed sent. A close after that drops no file frame: no error.
    usb_stream_closed = 19U,
    // More than usb_stall_timeout_ms between IN completions on the USB route
    // while armed, whether the gap has ended or not, or while the wait after
    // STOP still has file frames queued.
    usb_stall = 20U,
    // A PLAYLIST item on the USB route found no open capture stream at its
    // start, where a plain PLAY answers NO_STREAM and starts nothing: the item
    // played nothing. Only a playlist's result block reports it.
    usb_no_stream = 21U,
    // The play's cut stage refused: its log had no room for a cut it applied
    // (the log holds a cut for each entry a schedule may have, and the stage
    // applies each at most once), or the engine's output ran past 2^32 - 1
    // frames (no card reaches that). A schedule validate() accepted and a
    // card the player opens never cause it.
    cut_log = 22U,
    // A PLAYLIST item with dur= (a looped item) ends at its first anomaly,
    // where any other play goes on: a USB interval missed while it had frames
    // queued (uac_play_missed_intervals above 0) ...
    missed_interval = 23U,
    // ... usb_stack_asserts rising during it ...
    usb_assert = 24U,
    // ... or an engine deadline miss (USB? engine_deadline_misses).
    deadline_miss = 25U,
    // USB route: once no more file frames could join those queued (the file
    // had ended, or the play was disarmed by STOP or a fault), none of them
    // was confirmed sent for more than usb_stall_timeout_ms while IN
    // completions kept coming. The wait for them ends there instead of
    // hanging (#45); a host that stops polling is usb_stall first.
    usb_unconfirmed = 26U,
};

// A wire number as a protocol document publishes it: the enumerator and its
// number, for STATUS player_state and player_error
// (docs/live-control-protocol.md; host/live_control.py reads the end states
// 7, 8 and 255, and tests/test_live_control.py checks its copies against
// these tables), and engine_state and engine_error (error_map.hpp).
template <class Enum> struct WireNumber
{
    Enum value;
    std::uint32_t number;
};

template <class Enum, std::size_t Size> [[nodiscard]] constexpr bool wire_numbers_hold(const std::array<WireNumber<Enum>, Size>& published) noexcept
{
    return std::ranges::all_of(published, [](const WireNumber<Enum>& row) { return std::to_underlying(row.value) == row.number; });
}

inline constexpr auto published_player_states = std::to_array<WireNumber<PlayerState>>({
    {PlayerState::stopped, 0U},
    {PlayerState::waiting_for_command, 1U},
    {PlayerState::mounting, 2U},
    {PlayerState::opening, 3U},
    {PlayerState::buffering, 4U},
    {PlayerState::playing, 5U},
    {PlayerState::draining, 6U},
    {PlayerState::done, 7U},
    {PlayerState::aborted, 8U},
    {PlayerState::fault, 255U},
});

inline constexpr auto published_player_errors = std::to_array<WireNumber<PlayerError>>({
    {PlayerError::none, 0U},
    {PlayerError::host_init, 1U},
    {PlayerError::card_detect, 2U},
    {PlayerError::mount, 3U},
    {PlayerError::open, 4U},
    {PlayerError::header_io, 5U},
    {PlayerError::invalid_riff, 6U},
    {PlayerError::missing_format, 7U},
    {PlayerError::missing_data, 8U},
    {PlayerError::unsupported_format, 9U},
    {PlayerError::seek, 10U},
    {PlayerError::sd_read, 11U},
    {PlayerError::codec_config, 12U},
    {PlayerError::codec_level, 13U},
    {PlayerError::codec_start, 14U},
    {PlayerError::underrun, 15U},
    {PlayerError::impairment, 16U},
    {PlayerError::native_source, 17U},
    {PlayerError::live_control, 18U},
    {PlayerError::usb_stream_closed, 19U},
    {PlayerError::usb_stall, 20U},
    {PlayerError::usb_no_stream, 21U},
    {PlayerError::cut_log, 22U},
    {PlayerError::missed_interval, 23U},
    {PlayerError::usb_assert, 24U},
    {PlayerError::deadline_miss, 25U},
    {PlayerError::usb_unconfirmed, 26U},
});

// The player_error numbers this image gives no error. A number is never
// renumbered or reused: an image without the feature that raises an error
// keeps that number free (signal-lab-phase-2, which has no PLAYLIST and no
// looped items, reserves 21 and 23 to 25). This one assigns every number.
inline constexpr std::array<std::uint32_t, 0U> reserved_player_errors{};

// Whether `published` and `reserved` give each number from 0 to `last`'s
// once: the rows in increasing order, `last` the final one, and the reserved
// numbers, also increasing, in the gaps between them.
template <class Enum, std::size_t Rows, std::size_t Reserved>
[[nodiscard]] constexpr bool numbers_accounted_for(const std::array<WireNumber<Enum>, Rows>& published, const std::array<std::uint32_t, Reserved>& reserved, Enum last) noexcept
{
    auto row = published.begin();
    auto gap = reserved.begin();
    std::uint32_t number = 0U;

    for (; row != published.end() || gap != reserved.end(); ++number)
    {
        const bool published_here = row != published.end() && row->number == number;
        const bool reserved_here = gap != reserved.end() && *gap == number;

        if (published_here == reserved_here)
        {
            return false;
        }

        if (published_here)
        {
            ++row;
        }
        else
        {
            ++gap;
        }
    }

    return !published.empty() && published.back().value == last && published.back().number + 1U == number;
}

// Each published number is pinned, so renumbering an enumerator fails the
// build. Every number up to the last error's has its row or is reserved;
// usb_unconfirmed is the last error, and a new one goes after it, here too.
static_assert(wire_numbers_hold(published_player_states) && wire_numbers_hold(published_player_errors), "the published player states and errors");
static_assert(numbers_accounted_for(published_player_errors, reserved_player_errors, PlayerError::usb_unconfirmed),
              "a row for every player error number up to usb_unconfirmed's, in order, unless the number is reserved");

// A play on the USB route fails with usb_stall when a gap between IN
// completions exceeds this: a host that stops polling must not hang the play.
// The USB ISR judges a gap when the completion that ends it comes; the player
// task finds one that has not ended in its producer and drain loops.
inline constexpr std::uint32_t usb_stall_timeout_ms = 100U;

struct PlayerSnapshot
{
    std::uint32_t state;
    std::uint32_t error;
    std::uint32_t data_bytes;
    std::uint32_t file_frames_total;
    std::uint32_t file_frames_enqueued;
    std::uint32_t ring_fill_frames;
    std::uint32_t ring_min_frames;
    std::uint32_t ring_min_pre_eof_frames;
    std::uint32_t ring_max_frames;
    std::uint32_t frames_requested;
    std::uint32_t file_frames_submitted;
    std::uint32_t pcm_drained;
    std::uint32_t eof_count;
    std::uint32_t eof_silence_frames;
    std::uint32_t sd_read_calls;
    std::uint32_t sd_bytes_read; // holds at 2^32 - 1: a looped PLAYLIST item passes 4 GiB of reads after about 8.28 h
    std::uint32_t sd_short_reads;
    std::uint32_t sd_read_errors;
    std::uint32_t underruns;
    std::uint32_t first_underrun_frame;
    std::uint32_t max_sd_read_cycles;
    // Impairment engine (2:/WG/PLAY.SCN): an EngineState, 0 = no scenario (clean
    // pass-through), 1 = active, 2 = scenario rejected (playback continues clean),
    // 3 = processing failed (playback stops); see engine_error.
    std::uint32_t engine_state;
    std::uint32_t engine_error;
    std::uint32_t engine_stages;
    std::uint32_t engine_frames_in;
    std::uint32_t engine_frames_out;
    std::uint32_t engine_clipped;
    std::uint32_t engine_digest_hi;
    std::uint32_t engine_digest_lo;
    std::uint32_t engine_source_digest_hi;
    std::uint32_t engine_source_digest_lo;
    std::uint32_t engine_max_block_cycles;
    std::uint32_t engine_events_applied;
    std::uint32_t engine_events_dropped;
    std::uint32_t engine_arena_bytes;
    // Post-impairment output FIFO (OCRAM) and the producer that fills it.
    std::uint32_t ring_capacity_frames;
    std::uint32_t ring_high_watermark_frames;
    std::uint32_t ring_wake_watermark_frames;
    std::uint32_t ring_critical_frames;
    std::uint32_t ring_avg_frames;
    std::uint32_t ring_critical_events;
    std::uint32_t producer_rate_sps;
    std::uint32_t producer_worst_block_cycles;
    std::uint32_t producer_sleeps;
};

// STATUS engine_state (PlayerSnapshot::engine_state); error_map.hpp's
// published_engine_states pins the numbers.
enum class EngineState : std::uint8_t
{
    none = 0U,    // no scenario: a clean pass-through
    active = 1U,  // the scenario's engine runs
    refused = 2U, // the scenario was refused; the play goes on clean
    failed = 3U,  // the engine failed a block; the play stopped (player_error impairment)
};

enum class EngineError : std::uint32_t
{
    none = 0U,
    scenario_open = 1U,
    scenario_read = 2U,
    scenario_parse = 3U,
    missing_reference_rms = 4U,
    configure_failed = 5U,
    scenario_too_large = 6U,
    clipping_rejected = 7U,
    processing_failed = 8U,
};

/*
 * Start the engineering slice in an idle state. After the USB owner grants
 * local/FatFs ownership, request_player_play() opens 2:/WG/PLAY.WAV, accepts
 * packed PCM24/48 kHz/mono, prebuffers, then plays it once. Submitted frames are
 * copied into the SAI/eDMA buffer from the audio callback; done is not entered
 * until a bounded post-EOF silence drain has completed. This temporary control
 * surface predates WFG/1 and does not produce a qualified artifact.
 */
[[nodiscard]] m110::Status start_player() noexcept;
[[nodiscard]] m110::Status request_player_play() noexcept;
[[nodiscard]] bool player_allows_media_host() noexcept;
[[nodiscard]] PlayerSnapshot player_snapshot() noexcept;
// Current/last play's channel identity; read under the same task lock as counters.
[[nodiscard]] playlist::WattersonIdentity player_watterson_identity() noexcept;

// Measured second-source identity for the current or last accepted play;
// copied under the same task lock as the other player diagnostics.
[[nodiscard]] SecondarySourceIdentity player_secondary_source_identity() noexcept;

struct LiveSnapshot
{
    char selected_file[13]{};
    char run_id[33]{};
    bool native_source{};
    bool busy{};
    std::uint64_t seed{};
    double reference_rms{}; // Effective post-source-gain RMS used by live controls.
    std::uint64_t live_frame{};
    std::uint64_t live_digest{};
    std::uint64_t live_clipped{};
    std::uint32_t live_events{};
    std::uint32_t queue_free{};
    std::uint32_t capture_free{};
};

struct ControlReply
{
    std::uint32_t seq{};
    bool ok{};
    const char* error{"BAD_STATE"};
    std::uint64_t apply_frame{};
    std::uint32_t events{};
    // A refused PLAYLIST start: the playlist file's line it names (0 for the
    // file as a whole) and what the card check found there, when it says more
    // than the code (a static text, or none).
    std::uint32_t line{};
    const char* detail{};
    // A successful FILES? reply borrows the fixed player-owned page until the
    // USB task serializes it and another FILES? replaces it.
    const card_listing::Page* files{};
};

// Single fixed mailbox. The player task owns all source/engine/control mutations.
// Puts `command` in it: success, or refusal::busy (error_map.hpp; the code BUSY
// the reply carries) while a command or its reply is pending, or before the
// player task runs.
[[nodiscard]] m110::Status submit_player_command(const live_protocol::Request& command) noexcept;
[[nodiscard]] bool take_player_reply(ControlReply& reply) noexcept;
[[nodiscard]] LiveSnapshot player_live_snapshot() noexcept;
void notify_player_task() noexcept;

// The output route. Only the player task changes it, through OUTPUT CODEC and
// OUTPUT USB in its mailbox: naming the current route always succeeds and
// changes nothing, and a change needs a stopped player (else BUSY). Any task
// may read it. PLAY sends the file where it points. On the USB route PLAY needs
// an open capture stream (else NO_STREAM); the stream carries exact digital
// silence, then the file's frames once the output FIFO is prefilled, then
// silence again, and the run is done only once every file frame is confirmed
// sent. MEDIA HOST is refused while a play is armed. Every accepted PLAY, on
// either route, restarts the USB play statistics; on the codec route they stay
// zero (no USB play).
[[nodiscard]] OutputRoute player_output_route() noexcept;

// Where a play's first cut took effect: its place in the play's stream and,
// on the USB route, the stream index of the play's first frame, from which
// the cut's own stream index follows. USB? reports the two as one number
// (uac_play_cut_first_index).
struct FirstCut
{
    cut::SentFrames position{};                         // the play's frames sent before it
    std::optional<usb_audio::FrameCount> stream_base{}; // the USB play's uac_play_first_frame_index; none on the codec route
};

// The CUT injector (common/cut_schedule.hpp; docs/live-control-protocol.md).
// CUT sets the schedule the next PLAY takes, once cut::validate() accepts it
// (else CUT_RANGE, CUT_UNSORTED, CUT_OVERLAP or CUT_DROP_RUN, changing
// nothing); CUT alone
// empties it; BUSY while a run is active. An accepted PLAY takes the list
// (the play after it has none unless CUT is sent again); LOAD keeps it. Each
// PLAYLIST item sets its own list (cut=, or none) through the same latch, so
// a list set before PLAYLIST reaches no item. The
// player task applies it to the output of the scenario engine and the live
// controls, after the channel, on either route: the engine processes every
// card frame, a DROP discards frames of its output (engine time and the live
// controls' AT: positions run on through them), and a ZERO puts exact zero
// frames into the output FIFO, which the engine and the live controls never
// see. Offsets count the engine's output frames (a clean card's own frames).
// The current or last play's cuts, counted within the frames its route
// confirmed sent, as USB? reports them; every field restarts with each
// accepted PLAY and is 0 (or none) until the first.
struct PlayCutStatistics
{
    std::uint32_t applied{};         // uac_play_cut_count: cuts that took effect
    std::uint32_t drop_frames{};     // uac_play_cut_drop_frames: engine output frames they discarded
    std::uint32_t zero_frames{};     // uac_play_cut_zero_frames: zero frames sent
    std::optional<FirstCut> first{}; // uac_play_cut_first_index: where the first took effect; none while none has
    std::uint32_t rejected{};        // uac_play_cut_rejected: entries at or past the output's end, ignored; once the whole stream was sent
    std::uint32_t sent_frames{};     // uac_play_sent_frames: the play's frames confirmed sent (USB) or taken by the codec, after the cuts
    std::uint32_t usb_file_frames{}; // uac_play_file_frames: the engine output frames a USB play's sent frames cover; 0 for a codec play
    std::uint32_t output_frames{};   // the engine output frames the sent frames cover, on either route
};

// The current or last play's cuts. `usb_play` is the USB play's statistics,
// read in the same critical section as this call (it takes its own, which
// nests). Caller: any task.
[[nodiscard]] PlayCutStatistics player_cut_statistics(const usb_audio::PlayStatistics& usb_play) noexcept;

// uac_play_cut_first_index, the protocol's plain number: the stream index
// where the play's first cut took effect on the USB route (32 bits, wrapping
// as the stream's index does), its frames from the play's first on the codec
// route, and 0 while none has, which uac_play_cut_count 0 tells apart.
[[nodiscard]] constexpr std::uint32_t first_cut_wire_index(const std::optional<FirstCut>& first) noexcept
{
    if (!first.has_value())
    {
        return 0U;
    }

    return first->stream_base.value_or(usb_audio::FrameCount{}).value + first->position.value;
}

// Engine deadline misses of the current or last play, on either route (USB?
// engine_deadline_misses; 0 until the first accepted PLAY): pull steps whose
// SD read and engine block ended once the output, already running, had
// drained the output FIFO below ring_critical_frames (100 ms) before the
// stream's end, so the block came too late to keep the FIFO's margin. Any
// play counts them; a looped PLAYLIST item (dur=) ends on the first
// (PlayerError::deadline_miss). Caller: any task.
[[nodiscard]] std::uint32_t player_deadline_misses() noexcept;

// Why the current or last play's scenario was refused (STATUS engine_state 2)
// or its engine stopped (engine_state 3), as the Status that said so; success
// while neither has happened, and restarted by every accepted PLAY and
// PLAYLIST item. Its message is static text: the scenario parser's, the
// engine's or a stage's (signal-lab's messages), or the engine error's name
// (EngineError, rt1170/error_map.hpp) for a refusal with no other reason: a
// missing or unreadable file, a file over the read buffer, or no
// reference_rms. ENGINE? reports it, at most engine_reason_max_chars of it
// (status_contract.hpp), unless the live controls stopped the play. Caller:
// any task.
[[nodiscard]] m110::Status player_engine_reason() noexcept;

// Why the live controls stopped the current or last play (player_error 18,
// live_control), as the Status that said so; success until they do, and
// restarted as player_engine_reason() is. Its message is static text: the
// live controller's (signal-lab's: a block, a reset or a preset control it
// refused) or live_fault::no_reference (rt1170/error_map.hpp). Of several
// refusals in one play it is the first, the one whose live_control fault the
// run kept. ENGINE?
// reports it when they stopped the play (player_error 18), even after a
// refused scenario, and when player_engine_reason() has none. Caller: any
// task.
[[nodiscard]] m110::Status player_live_reason() noexcept;

// PLAYLIST batch mode (common/playlist.hpp; docs/live-control-protocol.md).
// PLAYLIST:<NAME>[:CONTINUE] (a stopped player, the card Local, no file
// generation) reads 2:/WG/<NAME>, parses every line and checks every item
// against the card (its WAV as PLAY validates one, its scenario as PLAY loads
// one) before anything plays; a refusal names the line and changes nothing.
// Accepted, the player task plays the items in order, each as LOAD, its CUT
// list and PLAY would, then waits its gap: on the USB route counted in stream
// frames, so a USB item after a USB item starts exactly
// playlist::next_start_index() frames into the stream; on the codec route in
// ticks. Each item's result block (its per-play statistics, as STATUS and
// USB? report the current or last play) is kept in a ring of
// playlist::result_capacity until PLAYLIST ACK frees it; a full ring pauses
// the playlist before its next item. STOP ends the item and the playlist.
// While a playlist runs every other run command (LOAD, CUT, OUTPUT, SEED,
// REFERENCE, PLAY, PLAYLIST, the live controls) is BUSY and MEDIA HOST is
// refused, gaps included.
// The progress, the ring's numbers and its oldest blocks, for PLAYLIST?, read
// in one critical section. Caller: any task.
[[nodiscard]] playlist::Snapshot player_playlist_snapshot() noexcept;

// PREPARE? reads a copy published by the player task after each bounded
// preparation step. No filesystem work runs in the querying task.
[[nodiscard]] prepare::Snapshot player_prepare_snapshot() noexcept;

// A copy of the current or last admitted PREPAREPLAY run. The player owns the
// coordinator; queries copy this publication under one lock, including its
// 64-bit watermarks. read_frames counts cumulative enqueues, not FIFO depth;
// consumed_frames counts FIFO departures, not confirmed USB delivery. STATUS?
// and USB? remain the transport evidence. operation_id is boot-local and
// starts at zero; ordinary PREPARE leaves the last admitted ID intact. timing
// is elapsed invocation time, including I/O waits and interrupts, not CPU time.
struct PreparedPlaybackDiagnostics
{
    finite_playback::Snapshot progress{};
    std::uint64_t operation_id{};
    operation_timing::Snapshot timing{};
    std::uint32_t measured_rate_frames_per_second{};
    std::uint32_t margin_frames{};
    bool active{};
    m110::Status status{m110::Status::success()};
};

[[nodiscard]] PreparedPlaybackDiagnostics player_prepared_playback_snapshot() noexcept;

// The USB route's source. Fills `frames` from the output FIFO: the leading
// frames carry the play's audio (the file after its cuts, a ZERO's zeros
// included) as L = R = sample << 8, every other frame is exact
// silence. Returns the file frames copied. Silence and zero after EOF, STOP or
// a fault; a FIFO that runs dry before EOF latches PlayerError::underrun.
// Caller: the USB ISR, while a play is armed (usb_audio.cpp); `frames` is
// borrowed for the call. Never blocks, allocates, logs or does I/O.
[[nodiscard]] usb_audio::FrameCount player_consume_from_isr(usb_audio::ChunkFrames frames) noexcept;

} // namespace waveform_generator
