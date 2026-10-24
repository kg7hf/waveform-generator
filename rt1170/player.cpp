// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// SD-to-output owner loop: the codec or the USB sound card's capture stream,
// one PLAY at a time or a PLAYLIST's items in turn
// Guide: docs/modules/rt1170-player-and-audio.md
// Ownership: Player task produces; the output route's consumer drains a bounded sample FIFO:
// the codec's audio task, or the USB ISR while a play on the USB route is armed.
// The player task is the single FatFs owner, the PLAYLIST sequencer included.
// Contract: Atomic indices publish sample ownership; short critical sections protect aggregate telemetry.

#include "player.hpp"

#include "error_map.hpp"
#include "tx_artifact.hpp"
#include <atomic>

#include "card_listing.hpp"
#include "common/attenuation.hpp"
#include "common/crc32.hpp"
#include "common/cut_schedule.hpp"
#include "common/live_command.hpp"
#include "common/pcm24.hpp"
#include "common/playlist.hpp"
#include "common/secondary_source_identity.hpp"
#include "common/wav.hpp"
#include "os/critical_section.hpp"
#include "platform/board.hpp"
#include "platform/waveform_msc.h"
#include "platform/wm8960_codec.hpp"
#include "prepare_job.hpp"
#include "prepared_playback.hpp"
#include "secondary_wav.hpp"
#include "signal_lab/mixer.hpp"
#include "signal_lab/scenario.hpp"
#include "signal_lab/second_source.hpp"
#include "usb_audio.hpp"

extern "C"
{
#include "FreeRTOS.h"
#include "ff.h"
#include "fsl_common.h"
#include "task.h"
}

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>

namespace waveform_generator
{
namespace
{

constexpr char drive_path[] = "2:/";
// The directory a selected file name is appended to (reset_selection).
constexpr std::string_view media_directory{"2:/WG/"};
char playback_path[24] = "2:/WG/PLAY.WAV";
char scenario_path[24] = "2:/WG/PLAY.SCN";
// A path in media_directory, as playback_path and scenario_path hold one.
constexpr std::size_t media_path_capacity = std::size(playback_path);
using MediaPath = std::array<char, media_path_capacity>;
static_assert(media_path_capacity == std::size(scenario_path) && media_directory.size() + playlist::name_max_chars < media_path_capacity,
              "a short name in media_directory fits a path with its terminator");
constexpr std::uint32_t scenario_capacity = 8192U;
// The impairment engine runs in this task; its deterministic math and the FatFs
// scenario read need more headroom than the original 1536-word stack.
constexpr std::uint32_t player_task_stack_words = 4096U;
constexpr UBaseType_t player_task_priority = 4U;
constexpr std::uint32_t codec_frames_per_block = 128U;
// Post-impairment output FIFO in OCRAM2: the only large buffer in the chain and the
// hard real-time boundary. The producer (SD read -> impairment engine) runs ahead of
// real time until the high watermark, sleeps until the wake watermark, and is never
// deadline-bound per block; only the audio hook draining this ring has a deadline.
constexpr std::uint32_t ring_capacity_frames = 32768U; // 683 ms
constexpr std::uint32_t ring_mask = ring_capacity_frames - 1U;
constexpr std::uint32_t ring_high_watermark_frames = 24000U; // 500 ms: stop producing
constexpr std::uint32_t ring_wake_watermark_frames = 14400U; // 300 ms: resume producing
constexpr std::uint32_t ring_critical_frames = 4800U;        // 100 ms: count as a near-miss
// One SD chunk = one engine block of packed little-endian PCM24 frames.
constexpr std::uint32_t read_buffer_bytes = 3U * signal_lab::engine_block_frames;
constexpr std::uint32_t eof_drain_frames = 3U * codec_frames_per_block;
// A read: one engine block of card frames.
constexpr std::uint32_t read_block_frames = read_buffer_bytes / audio::pcm24_bytes_per_sample;
// The cut stage's step (common/cut_schedule.hpp): at most one engine block's
// output into the output FIFO, of the engine's frames or of a ZERO's zeros.
constexpr std::uint32_t output_step_frames = signal_lab::engine_capacity_frames;
constexpr cut::StepLimit output_step = cut::StepLimit::of(output_step_frames).value();
// The producer paces itself in milliseconds of 48 kHz frames.
constexpr std::uint32_t frames_per_millisecond = 48U;

// A PCM24 sample left-justified in a 32-bit slot: the WM8960's 24 valid MSBs
// and the USB sound card's 24-in-32 subslot alike.
constexpr std::uint32_t slot_shift_bits = 8U;
constexpr std::size_t codec_channels = 2U;
// The output-route errors and the USB stall clock.
// The card's reads: a seek or a read that failed, or a read that came short.
constexpr m110::Status sd_seek_failed{.code = m110::StatusCode::io_error, .message = "SD seek failed"};
constexpr m110::Status sd_read_failed{.code = m110::StatusCode::io_error, .message = "SD read failed"};
constexpr TickType_t usb_stall_ticks = pdMS_TO_TICKS(usb_stall_timeout_ms);
constexpr usb_audio::TickSpan usb_stall_timeout{.value = usb_stall_ticks};
// The longest a USB play's end waits for the completion that settles its
// missed intervals (settle_play_misses()): one completion comes each
// millisecond while the host polls, and a 1-tick wait may end at the next
// tick, so two waits cover a whole millisecond.
constexpr std::uint32_t miss_settle_ms = 2U;

static_assert((ring_capacity_frames & ring_mask) == 0U);
static_assert((read_buffer_bytes % waveform_generator::audio::pcm24_bytes_per_sample) == 0U);
static_assert(std::is_same_v<TickType_t, std::uint32_t>, "the USB stall clock is the 32-bit RTOS tick");
static_assert(std::uint32_t{1} << slot_shift_bits == std::uint32_t{usb_audio::subslot_scale}, "codec and USB place a sample alike");
static_assert(ring_high_watermark_frames + 2U * signal_lab::engine_capacity_frames <= ring_capacity_frames, "a full engine block must always fit above the high watermark");
static_assert(ring_wake_watermark_frames < ring_high_watermark_frames && ring_critical_frames < ring_wake_watermark_frames);
static_assert(read_block_frames == signal_lab::engine_block_frames, "a read is one engine block");
static_assert(signal_lab::engine_capacity_frames <= ring_capacity_frames, "an engine block's output fits the FIFO's frame counts");
static_assert(ring_high_watermark_frames + 2U * output_step_frames <= ring_capacity_frames, "a cut stage step always fits above the high watermark");
// The engine's output of the longest card a WAV header can describe, at most
// engine_capacity_frames per block, stays within the cut stage's 32-bit
// places, so CutStage::offer() never refuses a block of a play.
static_assert((std::uint64_t{std::numeric_limits<std::uint32_t>::max() / audio::pcm24_bytes_per_sample} / read_block_frames + 1U) * signal_lab::engine_capacity_frames <=
                  std::numeric_limits<std::uint32_t>::max(),
              "the engine's output of any card fits 32-bit output frames");
// A looped PLAYLIST item (dur=) feeds the engine at most max_duration frames
// of its card; with every ZERO a schedule holds its stream still fits 32-bit
// frame counts. Its output passes 2^32 - 1 frames only if its scenario's
// sample slips add more than the headroom here, far past any slip; the cut
// stage then refuses the block (cut_log), so the play ends visibly instead
// of wrapping.
static_assert(std::uint64_t{playlist::max_duration.value} + std::uint64_t{cut::max_entries} * cut::max_length < std::numeric_limits<std::uint32_t>::max(),
              "a looped item's stream fits 32-bit frame counts");
// The producer's slowest output rate against real time that a CUT schedule's
// DROPs are budgeted for: engine output frames per frame the output plays.
// Image A measured 3.4 (producer_rate_sps 163720) for an AWGN, CW and fade
// scenario and 17 for a clean card.
constexpr std::uint32_t min_producer_speed = 2U;
// A DROP costs the producer engine time with nothing for the output FIFO
// (common/cut_schedule.hpp, the drop balance). A DROP begins with the FIFO
// at its wake watermark or above, less what the DROPs before it still owe:
// at min_producer_speed or faster, each frame discarded drains it by at most
// 1 / min_producer_speed frame and each frame sent refills it by at least
// as much, and the rest of the engine block that ends a DROP costs one more
// step. So within the drop balance validate() allows, the FIFO stays at or
// above ring_critical_frames: no deadline miss and no underrun.
static_assert((cut::max_drop_balance_frames + output_step_frames) / min_producer_speed <= ring_wake_watermark_frames - ring_critical_frames,
              "the output FIFO's margin covers the drop balance a CUT schedule may hold");

// Milliseconds as RTOS ticks (configTICK_RATE_HZ), converted as
// usb_stall_ticks is: the PLAYLIST sequencer's gaps, slack and sleeps are
// tick spans.
[[nodiscard]] constexpr usb_audio::TickSpan ticks_from_ms(std::uint32_t milliseconds) noexcept
{
    return {.value = pdMS_TO_TICKS(milliseconds)};
}

// PLAYLIST (common/playlist.hpp). The sequencer's longest sleep while it waits
// a gap or for the host's PLAYLIST ACK; a command wakes it at once.
constexpr std::uint32_t playlist_poll_ms = 100U;
constexpr usb_audio::TickSpan playlist_poll = ticks_from_ms(playlist_poll_ms);
// A scheduled start the tick clock shows passed more than this long ago, as
// after a pause for the result ring, is no start: the stream index it names
// could have wrapped since.
constexpr std::uint32_t schedule_slack_ms = 1000U;
constexpr usb_audio::TickSpan schedule_slack = ticks_from_ms(schedule_slack_ms);
static_assert(std::uint64_t{playlist::max_gap_milliseconds} * configTICK_RATE_HZ <= std::numeric_limits<TickType_t>::max(), "the longest gap is a tick span pdMS_TO_TICKS computes without overflow");
// A result block's start_tick and end_tick are RTOS ticks, which PLAYLIST?
// documents as milliseconds (common/playlist.hpp ResultBlock).
static_assert(configTICK_RATE_HZ == playlist::milliseconds_per_second, "PLAYLIST? reports RTOS ticks as milliseconds");
// Every player image is the composite device with the USB sound card.
constexpr playlist::OfferedRoutes offered_routes{.usb = true};
// The sequencer stopped: STOP while a playlist waits.
constexpr m110::Status playlist_stopped{.code = m110::StatusCode::busy, .message = "STOPPED"};
static_assert(playlist::chunk_frames == usb_audio::chunk_frames.value && playlist::frames_per_millisecond == frames_per_millisecond,
              "the sequencer counts the stream's packets and milliseconds as the player does");
static_assert(std::to_underlying(playlist::Route::codec) == std::to_underlying(OutputRoute::codec) && std::to_underlying(playlist::Route::usb) == std::to_underlying(OutputRoute::usb),
              "an item's route is the OUTPUT route of the same number");

// FatFs reads the whole sectors of a read straight into its destination, and
// the SD driver (fsl_sd) moves them by DMA only to a word-aligned address;
// to any other it reads one block per command through its own buffer. A
// sector boundary of the file lands on a word boundary of the destination
// when the destination's place within a word is the file pointer's. The
// producer reads the card in whole blocks into read_buffer (word-aligned)
// from the data chunk on, and seeks only back to the data chunk's first
// frame, at each seam of a looped PLAYLIST item: after the 44-byte header of
// a canonical card every read starts on a word boundary of the file.
constexpr std::uint32_t sd_dma_alignment_bytes = 4U;

static_assert(read_block_frames % sd_dma_alignment_bytes == 0U && read_buffer_bytes % sd_dma_alignment_bytes == 0U, "a whole block from an aligned file pointer ends aligned");

struct RuntimeCounters
{
    PlayerError error{PlayerError::none};
    std::uint32_t data_bytes{};
    std::uint32_t file_frames_total{};
    std::uint32_t file_frames_enqueued{};
    std::uint32_t ring_min_frames{ring_capacity_frames};
    std::uint32_t ring_min_pre_eof_frames{ring_capacity_frames};
    std::uint32_t ring_max_frames{};
    std::uint32_t frames_requested{};
    std::uint32_t file_frames_submitted{};
    std::uint32_t pcm_drained{};
    std::uint32_t eof_count{};
    std::uint32_t eof_silence_frames{};
    std::uint32_t sd_read_calls{};
    std::uint32_t sd_bytes_read{};
    std::uint32_t sd_short_reads{};
    std::uint32_t sd_read_errors{};
    std::uint32_t underruns{};
    std::uint32_t first_underrun_frame{};
    std::uint32_t max_sd_read_cycles{};
    // Output-FIFO depth statistics sampled by the audio hook, and producer pacing.
    std::uint64_t ring_fill_sum{};
    std::uint32_t ring_fill_samples{};
    std::uint32_t ring_critical_events{};
    bool ring_below_critical{};
    std::uint64_t producer_active_cycles{};
    std::uint32_t producer_frames{};
    std::uint32_t producer_worst_block_cycles{};
    std::uint32_t producer_sleeps{};
};

struct EngineCounters
{
    std::uint32_t state{};
    std::uint32_t error{};
    std::uint32_t stages{};
    std::uint32_t frames_in{};
    std::uint32_t frames_out{};
    std::uint32_t clipped{};
    std::uint32_t digest_hi{};
    std::uint32_t digest_lo{};
    std::uint32_t source_digest_hi{};
    std::uint32_t source_digest_lo{};
    std::uint32_t max_block_cycles{};
    std::uint32_t events_applied{};
    std::uint32_t events_dropped{};
    std::uint32_t arena_bytes{};
};

// The output FIFO lives in OCRAM2 (.ocram, NOLOAD; cmake/MIMXRT1176xxxxx_cm7_flexspi_nor_wfg.ld).
// Only the CPU touches it (producer writes, audio hook reads), so cacheability is harmless.
alignas(32) __attribute__((section(".ocram.output_ring"))) waveform_generator::audio::Pcm24Sample sample_ring[ring_capacity_frames];
alignas(32) std::array<std::uint8_t, read_buffer_bytes> read_buffer{};
// Engine contains bounded stage workspaces. Keep its storage in OCRAM2 (.ocram,
// NOLOAD); construct it in start_player(), before the owner task can run. The
// pointer remains live if task creation fails so a retry does not reconstruct it.
alignas(signal_lab::Engine) __attribute__((section(".ocram.player_engine"))) std::byte player_engine_storage[sizeof(signal_lab::Engine)];
signal_lab::Engine* player_engine{};
[[nodiscard]] signal_lab::Engine& engine() noexcept
{
    return *player_engine;
}

alignas(Rt1170SecondaryWav) __attribute__((section(".ocram.secondary_source"))) std::byte player_secondary_source_storage[sizeof(Rt1170SecondaryWav)];
Rt1170SecondaryWav* secondary_source_provider{};
[[nodiscard]] Rt1170SecondaryWav& player_secondary_source() noexcept
{
    return *secondary_source_provider;
}

alignas(PrepareJob) __attribute__((section(".ocram.prepare_job"))) std::byte player_prepare_storage[sizeof(PrepareJob)];
PrepareJob* preparation_job{};
alignas(32) __attribute__((section(".ocram.prepare_packed"))) std::array<std::uint8_t, 3U * signal_lab::engine_capacity_frames> prepare_packed;
prepare::Snapshot preparation_snapshot{};
std::atomic<bool> preparation_busy{};
signal_lab::Scenario scenario{};

// Startup does not clear OCRAM2. WattersonChannel::configure writes all state
// before processing (including after a rejected scenario); no large reset
// temporary or constructor runs on the player task's stack.
[[nodiscard]] signal_lab::WattersonChannel& player_watterson() noexcept
{
    __attribute__((section(".ocram.watterson"))) static constinit signal_lab::WattersonChannel channel{};
    return channel;
}

alignas(32) std::array<float, signal_lab::engine_capacity_frames> engine_input{};
alignas(32) std::array<float, signal_lab::engine_capacity_frames> engine_output{};
std::array<char, scenario_capacity> scenario_text{};
EngineCounters engine_counters{};
bool engine_active{};
StaticTask_t player_task_control{};
// The task stack and preset_events below are std::array: as C arrays with
// initializers they made lizard 1.24.0, which the complexity ratchet runs,
// lose every function after them (#49).
std::array<StackType_t, player_task_stack_words> player_task_stack{};
TaskHandle_t player_task_handle{};
FATFS file_system{};
FIL playback_file{};
// Telemetry is copied/mutated under task critical sections; the 64-bit fields
// must never be accessed concurrently outside that boundary on Cortex-M7.
RuntimeCounters counters{};
std::atomic<PlayerState> player_state{PlayerState::stopped};
static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
static_assert(std::atomic<bool>::is_always_lock_free);
static_assert(std::atomic<PlayerState>::is_always_lock_free);
std::atomic<std::uint32_t> ring_write_total{};
std::atomic<std::uint32_t> ring_read_total{};
std::atomic<bool> eof_enqueued{};
std::atomic<bool> faulted{};
std::atomic<bool> play_requested{};
std::atomic<bool> file_system_owned{};
bool card_listing_cleanup_pending{};
bool player_cleanup_pending{};
bool primary_close_pending{};
std::atomic<bool> run_busy{};
std::atomic<bool> stop_requested{};
bool playlist_preflight_active{};
bool playlist_preflight_reject_pending{};
signal_lab::LiveController live_controller{};
std::array<signal_lab::ControlEvent, signal_lab::live_capture_capacity> preset_events{};
LiveSnapshot live_snapshot{};
double reference_override{};
bool live_reference_valid{};
PreparedPlayback prepared_playback{};
PreparedPlaybackDiagnostics prepared_playback_diagnostics{};
std::uint64_t prepared_playback_operation_id{};
std::uint32_t prepared_consumed_frames{};
bool prepared_output_stopped{};
bool prepared_operation{};
bool prepared_failed{};
std::uint64_t live_seed{};
std::uint32_t run_number{};
std::atomic<bool> command_pending{};
std::atomic<bool> reply_pending{};

// The listing page is in OCRAM2 (.ocram, NOLOAD); read_page() clears and
// rebuilds it before the player publishes a successful reply.
[[nodiscard]] card_listing::Page& card_listing_page() noexcept
{
    __attribute__((section(".ocram.card_listing"))) static constinit card_listing::Page page{};
    return page;
}

// The player's command mailbox, in OCRAM2 (.ocram, NOLOAD: startup never
// writes it, so its initializer is not loaded): submit_player_command()
// writes it before it raises command_pending, and the player task reads it
// only then. A CUT request carries a whole schedule, so it stays out of DTCM.
[[nodiscard]] live_protocol::Request& command_mailbox() noexcept
{
    __attribute__((section(".ocram.player_command"))) static constinit live_protocol::Request mailbox{};
    return mailbox;
}

// The player's reply mailbox, in OCRAM2 (.ocram, NOLOAD) as the command
// mailbox is, out of DTCM: start_player() writes it before the player task
// exists; the task writes it before it raises reply_pending, and
// take_player_reply() reads it only then. Tasks alone touch it.
[[nodiscard]] ControlReply& reply_mailbox() noexcept
{
    __attribute__((section(".ocram.player_reply"))) static constinit ControlReply mailbox{};
    return mailbox;
}

// The CUT schedules (common/cut_schedule.hpp): the list CUT set for the next
// PLAY, the one the current or last play latched with its stage (after the
// engine), that play's log of the cuts it applied, and the route it ran on.
// The player task owns all of it; it changes the log and the route, and the
// USB task reads them (player_cut_statistics()), in critical sections.
struct PlayerCuts
{
    cut::ValidSchedule pending{};
    cut::CutStage stage{};
    cut::CutLog log{};
    OutputRoute route{OutputRoute::codec};
};

// In OCRAM2 (.ocram, NOLOAD), out of DTCM. Startup never writes it:
// start_player() does, before the player task exists, and the USB task reads
// it only once a PLAY has been accepted (play_accepted()).
[[nodiscard]] PlayerCuts& player_cuts() noexcept
{
    __attribute__((section(".ocram.player_cuts"))) static constinit PlayerCuts cuts{};
    return cuts;
}

// Whether a PLAY has been accepted since boot, so the cut log describes one.
// Caller: holds a critical section, as the PLAY that counts run_number does.
[[nodiscard]] bool play_accepted() noexcept
{
    return run_number != 0U;
}

// PLAYLIST (common/playlist.hpp): the items of the running or last playlist,
// its progress, the result blocks kept until the host acknowledges them, what
// the current item hands to its play (the scenario seed load_scenario()
// applies, the device-time start arm_usb_sink() passes on, the attenuation
// and the duration produce() applies), and the STOP that ends the playlist.
// The player task owns all of it; it writes the progress, the ring and
// owns_player in critical sections, and the USB task reads them in its own
// (player_playlist_snapshot(), player_allows_media_host()).
struct PlayerPlaylist
{
    playlist::ItemTable table{};
    playlist::Progress progress{};
    playlist::ResultRing results{};
    std::optional<std::uint32_t> scenario_seed{};
    std::optional<usb_audio::ScheduledStart> start{};
    attenuation::Tenths attenuation{};                  // the current item's att=; 0 dB for a plain PLAY
    std::optional<playlist::DurationFrames> duration{}; // the current item's dur=; none: its card once, and for a plain PLAY
    bool owns_player{};                                 // from an accepted PLAYLIST until it ends: every other run command is BUSY
    bool run_requested{};                               // accepted; the player task has not begun it yet
    bool stop_requested{};                              // STOP while the playlist runs
};

// In OCRAM2 (.ocram, NOLOAD), out of DTCM: startup never writes it, and
// start_player() resets it in place (reset_playlist()), before the player
// task exists; the table alone is 256 items.
[[nodiscard]] PlayerPlaylist& player_playlist() noexcept
{
    __attribute__((section(".ocram.playlist"))) static constinit PlayerPlaylist state{};
    return state;
}

// Every part of the playlist state as at boot, written in place: no
// temporary of the whole state ever lands on a stack.
void reset_playlist() noexcept
{
    auto& list = player_playlist();
    list.table.clear();
    list.results.clear();
    list.progress = playlist::Progress{};
    list.scenario_seed.reset();
    list.start.reset();
    list.attenuation = attenuation::none;
    list.duration.reset();
    list.owns_player = false;
    list.run_requested = false;
    list.stop_requested = false;
}

// Whether a playlist holds the player, gaps included. Caller: the player
// task, or a task in a critical section.
[[nodiscard]] bool playlist_owns_player() noexcept
{
    return player_playlist().owns_player;
}

// What the current or last play watches for: the engine deadline misses it
// has counted (player_deadline_misses(), USB? engine_deadline_misses) and,
// for a looped PLAYLIST item (dur=), which ends at its first anomaly, the
// TinyUSB assertion count when it began. The player task writes it, the
// count in critical sections; the USB task reads the count in its own.
struct PlayWatch
{
    playlist::WattersonIdentity watterson{};
    SecondarySourceIdentity secondary_source{};
    std::uint32_t deadline_misses{};
    std::uint32_t asserts_at_start{};
    bool looped{};
    // Why the play's scenario was refused or its engine stopped, the Status
    // that said so (player_engine_reason()); success until either happens.
    m110::Status engine_reason{};
    // Why the live controls stopped the play (live_control), the Status that
    // said so (player_live_reason()); success until they do.
    m110::Status live_reason{};
};

// In OCRAM2 (.ocram, NOLOAD), out of DTCM: start_player() writes it before
// the player task exists, and every accepted PLAY or item restarts it
// (begin_run()).
[[nodiscard]] PlayWatch& play_watch() noexcept
{
    __attribute__((section(".ocram.player_watch"))) static constinit PlayWatch watch{};
    return watch;
}

[[nodiscard]] constexpr OutputRoute to_output_route(playlist::Route route) noexcept
{
    return route == playlist::Route::usb ? OutputRoute::usb : OutputRoute::codec;
}

// The output route (OUTPUT CODEC / OUTPUT USB), CODEC at boot. The player
// task is its only writer: select() for OUTPUT from its mailbox, which
// enforces the stopped-player rule, and set_between_runs() for each PLAYLIST
// item, which the sequencer calls only between runs. Any task reads it
// through current(): the atomic is lock-free, so a reader never waits for the
// player.
class OutputRouteSelector
{
public:
    // Naming the current route always succeeds and changes nothing. A change
    // needs a stopped player; otherwise busy ("BUSY") and the route stays.
    [[nodiscard]] m110::Status select(OutputRoute requested) noexcept;

    // A PLAYLIST item's route. Caller: the sequencer, with no run busy.
    void set_between_runs(OutputRoute route) noexcept { route_.store(route); }

    [[nodiscard]] OutputRoute current() const noexcept { return route_.load(); }

private:
    static_assert(std::atomic<OutputRoute>::is_always_lock_free);
    std::atomic<OutputRoute> route_{OutputRoute::codec};
};

// Shared between the player task (select) and the USB task (STATUS, INFO);
// constant-initialized, so it is ready before any task starts.
OutputRouteSelector output_route{};

bool pending_audio_drain() noexcept
{
    taskENTER_CRITICAL();
    const bool pending = counters.eof_silence_frames < eof_drain_frames;
    taskEXIT_CRITICAL();
    return pending;
}

void service_player_command() noexcept;
void poll_secondary_source_command() noexcept;

void publish_live() noexcept
{
    taskENTER_CRITICAL();
    // A playlist holds the player in its gaps too: no file generation then.
    live_snapshot.busy = run_busy || playlist_owns_player() || preparation_busy || player_cleanup_pending;
    live_snapshot.seed = live_seed;
    live_snapshot.reference_rms = live_reference_valid ? live_controller.reference_rms() : 0.0;
    live_snapshot.live_frame = live_controller.frame();
    live_snapshot.live_digest = live_controller.stats().output_digest;
    live_snapshot.live_clipped = live_controller.stats().clipped_samples;
    live_snapshot.live_events = live_controller.stats().controls_applied;
    live_snapshot.queue_free = static_cast<std::uint32_t>(signal_lab::live_pending_capacity - live_controller.pending_count());
    live_snapshot.capture_free = static_cast<std::uint32_t>(signal_lab::live_capture_capacity - live_controller.capture_count());
    taskEXIT_CRITICAL();
}

std::uint32_t ring_fill() noexcept
{
    return ring_write_total - ring_read_total;
}

void set_state(PlayerState state) noexcept
{
    taskENTER_CRITICAL();
    player_state = state;
    taskEXIT_CRITICAL();
}

// The first fault of a run wins. CriticalSection is the caller's context:
// TaskCriticalSection in a task, IsrCriticalSection in the USB ISR.
template <class CriticalSection = TaskCriticalSection> void latch_fault(PlayerError error) noexcept
{
    const CriticalSection lock;

    if (faulted)
    {
        return;
    }

    faulted = true;
    counters.error = error;
    player_state = PlayerState::fault;
}

// `total` plus `more`, held at 2^32 - 1 once it gets there. sd_bytes_read
// counts with it: a looped PLAYLIST item reads its card pass after pass, so
// a play past about 8.28 h (4 GiB of PCM24) would otherwise wrap it.
[[nodiscard]] constexpr std::uint32_t saturating_sum(std::uint32_t total, std::uint32_t more) noexcept
{
    constexpr auto most = std::numeric_limits<std::uint32_t>::max();
    return more > most - total ? most : total + more;
}

static_assert(saturating_sum(1U, 2U) == 3U && saturating_sum(std::numeric_limits<std::uint32_t>::max() - 1U, 1U) == std::numeric_limits<std::uint32_t>::max() &&
                  saturating_sum(std::numeric_limits<std::uint32_t>::max() - 1U, 2U) == std::numeric_limits<std::uint32_t>::max(),
              "a sum below 2^32 is exact, and one past it holds at 2^32 - 1");

void record_read(FRESULT result, UINT requested, UINT actual, std::uint32_t elapsed_cycles) noexcept
{
    taskENTER_CRITICAL();
    counters.sd_read_calls = counters.sd_read_calls + 1U;
    counters.sd_bytes_read = saturating_sum(counters.sd_bytes_read, actual);

    if (result != FR_OK)
    {
        counters.sd_read_errors = counters.sd_read_errors + 1U;
    }
    else if (actual != requested)
    {
        counters.sd_short_reads = counters.sd_short_reads + 1U;
    }

    if (elapsed_cycles > counters.max_sd_read_cycles)
    {
        counters.max_sd_read_cycles = elapsed_cycles;
    }

    taskEXIT_CRITICAL();
}

// Reads `bytes` of playback_file into `destination`, counted in the sd_*
// counters: success, or sd_read_failed for a failed or short read.
[[nodiscard]] m110::Status read_exact(void* destination, UINT bytes) noexcept
{
    UINT bytes_read{};
    const auto begin = m110::platform_cycle_counter();
    const auto result = f_read(&playback_file, destination, bytes, &bytes_read);
    const auto elapsed = m110::platform_cycle_counter() - begin;
    record_read(result, bytes, bytes_read, elapsed);
    return result == FR_OK && bytes_read == bytes ? m110::Status::success() : sd_read_failed;
}

// Moves playback_file to `offset`: success, or sd_seek_failed.
[[nodiscard]] m110::Status seek_to(std::uint32_t offset) noexcept
{
    return f_lseek(&playback_file, static_cast<FSIZE_t>(offset)) == FR_OK ? m110::Status::success() : sd_seek_failed;
}

// The WAV reader's callback for PLAY: the bytes at `offset` of the open
// playback_file, counted in the sd_* counters: success, sd_seek_failed or
// sd_read_failed.
[[nodiscard]] m110::Status read_wav_bytes(void*, std::uint32_t offset, std::uint8_t* destination, std::uint32_t bytes) noexcept
{
    if (const m110::Status sought = seek_to(offset); !sought.is_ok())
    {
        return sought;
    }

    return read_exact(destination, static_cast<UINT>(bytes));
}

// As read_wav_bytes(), for a PLAYLIST start's card checks: the read counts in
// none of the sd_* counters, which describe the current or last play (STATUS),
// so a refused PLAYLIST leaves STATUS as it was.
[[nodiscard]] m110::Status read_wav_bytes_uncounted(void*, std::uint32_t offset, std::uint8_t* destination, std::uint32_t bytes) noexcept
{
    if (const m110::Status sought = seek_to(offset); !sought.is_ok())
    {
        return sought;
    }

    UINT bytes_read{};
    return f_read(&playback_file, destination, static_cast<UINT>(bytes), &bytes_read) == FR_OK && bytes_read == bytes ? m110::Status::success() : sd_read_failed;
}

// Whether a WAV header's reads count in the run's sd_* counters: PLAY's do,
// a PLAYLIST start's card checks' do not.
enum class ReadCounting : std::uint8_t
{
    counted,
    uncounted,
};

// A file's size as FatFs reports it (FSIZE_t, 64 bits with exFAT), at most
// 32 bits: a larger file is no WAV the player plays (its RIFF sizes are 32
// bits) and no playlist it reads. FatFs's type: this boundary's one cast.
[[nodiscard]] std::uint32_t file_size_bytes(FIL& file) noexcept
{
    const FSIZE_t size = f_size(&file);
    return std::cmp_greater(size, std::numeric_limits<std::uint32_t>::max()) ? std::numeric_limits<std::uint32_t>::max() : static_cast<std::uint32_t>(size);
}

// The WAV the open playback_file holds, checked as PLAY checks it (mono
// 48 kHz packed PCM24), without latching a fault: PLAY and the PLAYLIST card
// checks share it, PLAY's reads counted, the checks' not.
[[nodiscard]] WavValidationError validate_open_wav(WavPayload& payload, ReadCounting counting) noexcept
{
    const WavReader reader{
        .context = nullptr,
        .file_size = file_size_bytes(playback_file),
        .read = counting == ReadCounting::counted ? &read_wav_bytes : &read_wav_bytes_uncounted,
    };
    return validate_pcm24_mono_48k_wav(reader, payload);
}

// PLAY's check of the open playback_file: success with `payload` and the
// counters set, or the Status of the player error a refused WAV latches.
[[nodiscard]] m110::Status parse_wav(WavPayload& payload) noexcept
{
    const auto result = validate_open_wav(payload, ReadCounting::counted);

    if (result != WavValidationError::none)
    {
        const PlayerError error = player_error_of(result);
        latch_fault(error);
        return status_of(error);
    }

    taskENTER_CRITICAL();
    counters.data_bytes = payload.bytes;
    counters.file_frames_total = payload.bytes / audio::pcm24_bytes_per_sample;
    taskEXIT_CRITICAL();
    return m110::Status::success();
}

// A PCM24 sample in bits 31..8 of a slot, the low byte zero. Unsigned, so the
// shift is defined for every sample, as the codec path always computed it.
[[nodiscard]] constexpr std::uint32_t slot_word(audio::Pcm24Sample sample) noexcept
{
    return std::bit_cast<std::uint32_t>(sample) << slot_shift_bits;
}

// One codec block: interleaved left/right 32-bit I2S words, both channels the
// same sample.
class CodecBlock
{
public:
    // The codec hook's view: two words per frame, valid for the hook call. The
    // codec runs codec_frames_per_block-frame blocks, so the count fits.
    CodecBlock(std::uint32_t* words, std::size_t frames) noexcept : words_{words, codec_channels * frames}, frames_{static_cast<std::uint32_t>(frames)} {}

    [[nodiscard]] std::uint32_t frames() const noexcept { return frames_; }

    void put(std::uint32_t frame, audio::Pcm24Sample sample) noexcept
    {
        const std::uint32_t word = slot_word(sample);
        words_[codec_channels * frame] = word;
        words_[codec_channels * frame + 1U] = word;
    }

    void silence_from(std::uint32_t first_frame) noexcept { std::ranges::fill(words_.subspan(codec_channels * first_frame), 0U); }

private:
    std::span<std::uint32_t> words_;
    std::uint32_t frames_;
};

// One USB chunk: stereo frames of 24-bit samples in 4-byte subslots, L = R.
class UsbChunk
{
public:
    explicit UsbChunk(usb_audio::ChunkFrames frames) noexcept : frames_{frames} {}

    [[nodiscard]] static constexpr std::uint32_t frames() noexcept { return usb_audio::chunk_frames.value; }

    void put(std::uint32_t frame, audio::Pcm24Sample sample) noexcept
    {
        const auto subslot = std::bit_cast<std::int32_t>(slot_word(sample));
        frames_[frame] = usb_audio::StereoFrame{.left = subslot, .right = subslot};
    }

    void silence_from(std::uint32_t first_frame) noexcept { std::ranges::fill(frames_.subspan(first_frame), usb_audio::StereoFrame{}); }

private:
    usb_audio::ChunkFrames frames_;
};

// Why a consumed block carries no file audio.
enum class BlockSilence : std::uint8_t
{
    stopped,   // STOP or a fault
    after_eof, // the file has ended: part of the EOF drain
};

template <class CriticalSection> void count_silent_block(std::uint32_t frames, BlockSilence silence) noexcept
{
    const CriticalSection lock;
    counters.frames_requested = counters.frames_requested + frames;

    if (silence == BlockSilence::after_eof)
    {
        counters.eof_silence_frames = counters.eof_silence_frames + frames;
    }
}

template <class CriticalSection> void count_underrun(std::uint32_t frames) noexcept
{
    const CriticalSection lock;
    counters.underruns = counters.underruns + 1U;

    if (counters.underruns == 1U)
    {
        counters.first_underrun_frame = counters.frames_requested;
    }

    counters.frames_requested = counters.frames_requested + frames;
}

// Depth statistics: one sample per consumed block gives the average depth and
// counts each excursion below the critical level (before EOF) as one event.
// Caller: holds a critical section.
void note_ring_depth(std::uint32_t fill_after) noexcept
{
    counters.ring_min_frames = std::min(counters.ring_min_frames, fill_after);
    counters.ring_fill_sum = counters.ring_fill_sum + fill_after;
    counters.ring_fill_samples = counters.ring_fill_samples + 1U;

    if (eof_enqueued)
    {
        return;
    }

    counters.ring_min_pre_eof_frames = std::min(counters.ring_min_pre_eof_frames, fill_after);
    const bool below_critical = fill_after < ring_critical_frames;

    if (below_critical && !counters.ring_below_critical)
    {
        counters.ring_critical_events = counters.ring_critical_events + 1U;
    }

    counters.ring_below_critical = below_critical;
}

template <class CriticalSection> void publish_consumed(std::uint32_t read_start, std::uint32_t copied, std::uint32_t frames) noexcept
{
    const CriticalSection lock;
    ring_read_total = read_start + copied;
    counters.frames_requested = counters.frames_requested + frames;
    counters.file_frames_submitted = counters.file_frames_submitted + copied;
    note_ring_depth(ring_write_total - ring_read_total);

    if (copied < frames)
    {
        counters.eof_silence_frames = counters.eof_silence_frames + frames - copied;
        counters.eof_count = counters.eof_count + 1U;
        player_state = PlayerState::draining;
    }
}

template <class Output> void copy_from_ring(Output& output, std::uint32_t read_start, std::uint32_t copied) noexcept
{
    const std::span<const audio::Pcm24Sample, ring_capacity_frames> ring{sample_ring};

    for (std::uint32_t frame = 0U; frame < copied; ++frame)
    {
        output.put(frame, ring[(read_start + frame) & ring_mask]);
    }
}

// Fills one codec block or USB chunk from the output FIFO and returns the file
// frames copied; they lead the block and every other frame is exact silence.
// CriticalSection is the caller's context: TaskCriticalSection in the codec's
// audio task, IsrCriticalSection in the USB ISR.
template <class CriticalSection, class Output> std::uint32_t consume_output(Output& output) noexcept
{
    const std::uint32_t frames = output.frames();

    if (faulted || stop_requested)
    {
        output.silence_from(0U);
        count_silent_block<CriticalSection>(frames, BlockSilence::stopped);
        return 0U;
    }

    if (player_state == PlayerState::draining || player_state == PlayerState::done)
    {
        output.silence_from(0U);
        count_silent_block<CriticalSection>(frames, BlockSilence::after_eof);
        return 0U;
    }

    const auto read_start = ring_read_total.load();
    const auto write_snapshot = ring_write_total.load();
    __DMB();
    const std::uint32_t copied = std::min(write_snapshot - read_start, frames);

    if (copied < frames && !eof_enqueued)
    {
        output.silence_from(0U);
        count_underrun<CriticalSection>(frames);
        latch_fault<CriticalSection>(PlayerError::underrun);
        return 0U;
    }

    copy_from_ring(output, read_start, copied);
    output.silence_from(copied);
    __DMB();
    publish_consumed<CriticalSection>(read_start, copied, frames);
    return copied;
}

void player_audio_hook(void*, const std::uint32_t*, std::uint32_t* playback, std::size_t frames) noexcept
{
    CodecBlock block{playback, frames};
    consume_output<TaskCriticalSection>(block);
}

// Latches `error` as the run's fault: its Status.
[[nodiscard]] m110::Status fault_of(PlayerError error) noexcept
{
    latch_fault(error);
    return status_of(error);
}

// Configures the codec once, sets its level and starts it: success, or the
// Status of the codec_config, codec_level or codec_start fault it latches.
[[nodiscard]] m110::Status start_codec() noexcept
{
    auto& codec = m110::imxrt1170::wm8960_codec();
    m110::imxrt1170::CodecConfig configuration;
    configuration.frames_per_block = codec_frames_per_block;
    const auto configured = codec.is_configured() ? m110::Status::success() : codec.configure(configuration, &player_audio_hook, nullptr);

    if (!configured.is_ok())
    {
        return fault_of(PlayerError::codec_config);
    }

    const auto level = codec.set_transmit_level_percent(70U);

    if (!level)
    {
        return fault_of(PlayerError::codec_level);
    }

    taskENTER_CRITICAL();
    counters.ring_min_frames = ring_write_total - ring_read_total;
    counters.ring_min_pre_eof_frames = ring_write_total - ring_read_total;
    taskEXIT_CRITICAL();
    set_state(PlayerState::playing);
    const auto started = codec.start();

    if (!started.is_ok())
    {
        return fault_of(PlayerError::codec_start);
    }

    return m110::Status::success();
}

[[nodiscard]] bool usb_route() noexcept
{
    return output_route.current() == OutputRoute::usb;
}

[[nodiscard]] usb_audio::TickCount current_tick() noexcept
{
    return usb_audio::TickCount{.value = xTaskGetTickCount()};
}

// The USB stream's play statistics; they belong to the USB interrupt level.
[[nodiscard]] usb_audio::PlayStatistics usb_play_statistics() noexcept
{
    const TaskCriticalSection lock;
    return usb_audio::play_statistics();
}

// Whether a completion reported a size other than its packet while the USB
// play had file frames queued (usb_audio::play_mismatched()).
[[nodiscard]] bool usb_play_mismatched() noexcept
{
    const TaskCriticalSection lock;
    return usb_audio::play_mismatched();
}

// No IN completion for usb_stall_ticks. Read `play` before `now`: a completion
// tick is never later than a tick read after it.
[[nodiscard]] bool usb_stalled(const usb_audio::PlayStatistics& play, usb_audio::TickCount now) noexcept
{
    return now.value - play.progress_tick.value > usb_stall_timeout.value;
}

// A play on the USB route as the player task judges it: the stream's
// statistics, and whether the file is delivered, that is the output FIFO
// reached EOF (draining) and every file frame taken from it was confirmed
// sent. The USB ISR changes both, so they are read in one critical section.
struct UsbPlayView
{
    usb_audio::PlayStatistics play{};
    bool delivered{};
};

[[nodiscard]] UsbPlayView usb_play_view() noexcept
{
    const TaskCriticalSection lock;
    const auto play = usb_audio::play_statistics();
    const bool delivered = player_state == PlayerState::draining && play.file_frames.value == counters.file_frames_submitted;
    return {.play = play, .delivered = delivered};
}

// PLAY on the USB route needs an open stream (else "NO_STREAM") and restarts
// the play statistics at the stream's current frame index. The codec route
// needs nothing here; it clears them, so STATUS reports no USB play rather than
// the last one. Either way chunks an earlier USB play left queued count for no
// play from here on.
[[nodiscard]] m110::Status accept_output() noexcept
{
    const TaskCriticalSection lock;

    if (!usb_route())
    {
        usb_audio::clear_play();
        return m110::Status::success();
    }

    return usb_audio::accept_play();
}

// From the next chunk on, the USB ISR takes file frames from the output FIFO.
// The ring statistics, the playing state and the armed stream change in one
// critical section, so the ISR never sees one without the others.
[[nodiscard]] m110::Status arm_usb_sink(usb_audio::TickCount now) noexcept
{
    const TaskCriticalSection lock;
    // A PLAYLIST item after a USB item starts at the stream index its gap ends at.
    const auto armed = usb_audio::arm_play(now, usb_stall_timeout, player_playlist().start);

    if (armed.is_ok())
    {
        counters.ring_min_frames = ring_write_total - ring_read_total;
        counters.ring_min_pre_eof_frames = ring_write_total - ring_read_total;
        player_state = PlayerState::playing;
    }

    return armed;
}

// Starts the output route once the FIFO holds its prefill. A failure is latched
// as the run's fault: the codec's own errors (start_codec()'s Status), or
// usb_stream_closed when the stream accepted for this play has closed since.
[[nodiscard]] m110::Status start_output() noexcept
{
    if (!usb_route())
    {
        return start_codec();
    }

    const auto armed = arm_usb_sink(current_tick());

    if (!armed.is_ok())
    {
        latch_fault(PlayerError::usb_stream_closed);
    }

    return armed;
}

// What ends a play on the USB route early: its stream closed before the file
// was delivered (error 19; a close after that drops no file frame), or, while
// armed, more than usb_stall_timeout_ms between IN completions: a gap the USB
// ISR saw end (stalled), or one still going on at `now` (error 20).
[[nodiscard]] std::optional<PlayerError> usb_sink_fault(const UsbPlayView& view, usb_audio::TickCount now) noexcept
{
    if (view.play.interrupted && !view.delivered)
    {
        return PlayerError::usb_stream_closed;
    }

    if (view.play.stalled || (view.play.armed && usb_stalled(view.play, now)))
    {
        return PlayerError::usb_stall;
    }

    return std::nullopt;
}

// A looped PLAYLIST item's anomalies beyond the faults that end every play
// (an underrun, an SD read error, a stream close or stall): a USB interval
// missed while it had frames queued, once the next completion has confirmed
// it (usb_audio::confirmed_missed(): a late read is taken back, not a miss),
// a completion reported short of its packet while it had frames queued
// (usb_unconfirmed; any other play runs on, judge_usb_delivery()), or
// usb_stack_asserts above their count when it began.
// note_block_deadline() judges an engine deadline miss where the block
// comes. None for any other play.
[[nodiscard]] std::optional<PlayerError> looped_anomaly() noexcept
{
    const auto& watch = play_watch();

    if (!watch.looped)
    {
        return std::nullopt;
    }

    if (usb_route() && usb_audio::confirmed_missed(usb_play_statistics()) != usb_audio::IntervalCount{})
    {
        return PlayerError::missed_interval;
    }

    if (usb_route() && usb_play_mismatched())
    {
        return PlayerError::usb_unconfirmed;
    }

    if (usb_audio::stack_asserts().count > watch.asserts_at_start)
    {
        return PlayerError::usb_assert;
    }

    return std::nullopt;
}

// The USB sink's faults (the codec route reports its errors itself). The
// first fault a run latches is its error.
void check_sink() noexcept
{
    const auto fault = usb_route() ? usb_sink_fault(usb_play_view(), current_tick()) : std::nullopt;

    if (fault.has_value())
    {
        latch_fault(*fault);
    }
}

// The player task's check of the output while a run is active: the USB
// sink's faults, then a looped item's anomalies.
void check_output() noexcept
{
    check_sink();
    const auto anomaly = looped_anomaly();

    if (anomaly.has_value())
    {
        latch_fault(*anomaly);
    }
}

// Output still draining after EOF: the codec's EOF silence blocks, or on the
// USB route file frames queued and not yet confirmed sent.
[[nodiscard]] bool pending_output_drain() noexcept
{
    if (!usb_route())
    {
        return pending_audio_drain();
    }

    return usb_play_statistics().pending_frames != usb_audio::FrameCount{};
}

// A USB play's file frames still queued, as the player task waits for them
// once no more can join them: the file has ended (draining), or the play is
// disarmed. Each chunk confirmed sent lowers them, so frames that have not
// fallen for more than usb_stall_timeout while IN completions still come
// are not going out: the waits end there with usb_unconfirmed (#45) instead
// of hanging. They can fall at most once per chunk record, so a wait never
// exceeds chunk_record_capacity such spans. A host that stops polling right
// after a fall is a stall (error 20) first: the player sees the fall no
// earlier than the completion that confirmed it.
class QueuedFramesWatch
{
public:
    // Whether `play`'s queued file frames, as just read, have not fallen for
    // more than usb_stall_timeout by `now`. The first call sees them fall.
    [[nodiscard]] bool stuck(const usb_audio::PlayStatistics& play, usb_audio::TickCount now) noexcept
    {
        if (!seen_ || play.pending_frames != pending_)
        {
            seen_ = true;
            pending_ = play.pending_frames;
            fell_ = now;
            return false;
        }

        return now.value - fell_.value > usb_stall_timeout.value;
    }

private:
    usb_audio::FrameCount pending_{};
    usb_audio::TickCount fell_{};
    bool seen_{};
};

QueuedFramesWatch prepared_drain{};

// While a USB play drains (its file has ended), its queued file frames must
// keep falling (QueuedFramesWatch). The codec route drains its EOF silence
// itself.
void check_drain(QueuedFramesWatch& watch) noexcept
{
    if (!usb_route() || player_state != PlayerState::draining)
    {
        return;
    }

    if (watch.stuck(usb_play_statistics(), current_tick()))
    {
        latch_fault(PlayerError::usb_unconfirmed);
    }
}

// Publishes `frames` frames written to the output FIFO from `write_start` on,
// and the FIFO's greatest depth. The frames are in place before the index
// moves (the barrier), so a consumer never reads one early.
void publish_written(std::uint32_t write_start, std::uint32_t frames) noexcept
{
    __DMB();
    const TaskCriticalSection lock;
    ring_write_total = write_start + frames;
    counters.file_frames_enqueued = counters.file_frames_enqueued + frames;
    counters.ring_max_frames = std::max(counters.ring_max_frames, ring_write_total - ring_read_total);
}

// Writes the first frames of a block to the output FIFO, as many as its free
// space allows before the ring wraps, and returns how many: frame `frame` is
// sample_at(frame). Caller: the player task, the FIFO's only writer.
template <class SampleAt> [[nodiscard]] std::uint32_t write_ring(std::uint32_t frames, const SampleAt& sample_at) noexcept
{
    const auto write_start = ring_write_total.load();
    const auto used = write_start - ring_read_total.load();
    const auto ring_offset = write_start & ring_mask;
    const std::uint32_t free_frames = used >= ring_capacity_frames ? 0U : ring_capacity_frames - used;
    const std::uint32_t count = std::min({frames, free_frames, ring_capacity_frames - ring_offset});

    if (count == 0U)
    {
        return 0U;
    }

    const std::span<audio::Pcm24Sample, ring_capacity_frames> ring{sample_ring};

    for (std::uint32_t frame = 0U; frame < count; ++frame)
    {
        ring[ring_offset + frame] = sample_at(frame);
    }

    publish_written(write_start, count);
    return count;
}

// Writes every frame of a block to the output FIFO, waiting while it is full,
// unless STOP or a fault ends the run first.
template <class SampleAt> void push_block(std::uint32_t frames, const SampleAt& sample_at) noexcept
{
    std::uint32_t written = 0U;

    while (written < frames && !faulted && !stop_requested)
    {
        const auto pushed = write_ring(frames - written, [&sample_at, written](std::uint32_t frame) { return sample_at(written + frame); });

        if (pushed == 0U)
        {
            vTaskDelay(1);
            continue;
        }

        written += pushed;
    }
}

// The play's card as the producer reads it: whole blocks from the data
// chunk's first frame on, the file pointer moving only by these reads and,
// for a looped PLAYLIST item (dur=), by the seek back to the data chunk's
// first frame at each seam, so loop n + 1's frame 0 follows loop n's last
// frame. The engine takes the frames as one input: its frame clock and its
// scenario's events run on across the seams. The cuts act on the engine's
// output, so the reads never depend on them.
struct CardSource
{
    std::uint32_t frames_left{}; // the play's input frames not yet read: the card's, or a looped item's duration
    std::uint32_t pass_left{};   // frames not yet read of the card's current pass
    std::uint32_t card_frames{}; // the card's frames: one pass
    std::uint32_t data_offset{}; // the data chunk's first frame in the file
};

// The input frames of a play of a card of `card_frames` frames: the card
// once, or a looped item's `duration` (none of an empty card, which the WAV
// checks refuse anyway).
[[nodiscard]] constexpr std::uint32_t play_input_frames(std::uint32_t card_frames, std::optional<playlist::DurationFrames> duration) noexcept
{
    if (card_frames == 0U || !duration.has_value())
    {
        return card_frames;
    }

    return duration->value;
}

// The card of the open playback_file, `payload`, as a play reads it: the
// card once, or as many frames of it looped as the current PLAYLIST item's
// dur= gives.
[[nodiscard]] CardSource card_source(const WavPayload& payload) noexcept
{
    const std::uint32_t card_frames = payload.bytes / audio::pcm24_bytes_per_sample;
    return {.frames_left = play_input_frames(card_frames, player_playlist().duration), .pass_left = card_frames, .card_frames = card_frames, .data_offset = payload.offset};
}

// A seam: the card's pass has ended with input frames left, so the next pass
// begins at the data chunk's first frame. A failed seek latches seek, an SD
// read error of the play.
[[nodiscard]] m110::Status begin_next_pass(CardSource& card) noexcept
{
    if (const m110::Status sought = seek_to(card.data_offset); !sought.is_ok())
    {
        latch_fault(PlayerError::seek);
        return sought;
    }

    card.pass_left = card.card_frames;
    return m110::Status::success();
}

// Reads the card's next block, at most read_block_frames frames and never
// past the end of its pass, into read_buffer: the bytes read, or the sd_read
// fault a failed or short read latches. Caller: frames are left to read.
[[nodiscard]] m110::Result<std::span<const std::uint8_t>> read_card_block(CardSource& card) noexcept
{
    if (card.pass_left == 0U)
    {
        if (const auto seam = begin_next_pass(card); !seam.is_ok())
        {
            return seam;
        }
    }

    const std::uint32_t frames = std::min({card.frames_left, card.pass_left, read_block_frames});
    const auto bytes = std::span{read_buffer}.first(std::size_t{frames} * audio::pcm24_bytes_per_sample);
    const auto requested = static_cast<UINT>(bytes.size());
    UINT actual{};
    const auto begin = m110::platform_cycle_counter();
    const auto result = f_read(&playback_file, bytes.data(), requested, &actual);
    const auto elapsed = m110::platform_cycle_counter() - begin;
    record_read(result, requested, actual, elapsed);

    if (result != FR_OK || actual != requested)
    {
        latch_fault(PlayerError::sd_read);
        return sd_read_failed;
    }

    card.frames_left -= frames;
    card.pass_left -= frames;
    return std::span<const std::uint8_t>{bytes};
}

// The play's scenario was refused: STATUS engine_state refused (2) and
// engine_error, with `reason` kept for ENGINE?.
void set_engine_error(EngineError error, m110::Status reason) noexcept
{
    taskENTER_CRITICAL();
    engine_counters.state = std::to_underlying(EngineState::refused);
    engine_counters.error = std::to_underlying(error);
    play_watch().engine_reason = reason;
    taskEXIT_CRITICAL();
}

void publish_engine_counters(std::uint32_t block_cycles) noexcept
{
    const signal_lab::EngineStats& stats = engine().stats();
    taskENTER_CRITICAL();
    engine_counters.frames_in = static_cast<std::uint32_t>(stats.frames_in);
    engine_counters.frames_out = static_cast<std::uint32_t>(stats.frames_out);
    engine_counters.clipped = static_cast<std::uint32_t>(stats.clipped_samples);
    engine_counters.digest_hi = static_cast<std::uint32_t>(stats.output_digest >> 32U);
    engine_counters.digest_lo = static_cast<std::uint32_t>(stats.output_digest & 0xFFFFFFFFU);
    engine_counters.source_digest_hi = static_cast<std::uint32_t>(stats.source_digest >> 32U);
    engine_counters.source_digest_lo = static_cast<std::uint32_t>(stats.source_digest & 0xFFFFFFFFU);
    engine_counters.events_applied = stats.events_applied;
    engine_counters.events_dropped = stats.events_dropped;

    if (block_cycles > engine_counters.max_block_cycles)
    {
        engine_counters.max_block_cycles = block_cycles;
    }

    taskEXIT_CRITICAL();
}

FIL scenario_file{}; // static: a FIL carries a sector buffer and must not live on the task stack

// The open scenario file's text, read into scenario_text and NUL-terminated
// for the parser, with the file closed: the text, or why it was not read.
struct ScenarioText
{
    std::string_view text{};
    EngineError error{EngineError::none};
};

[[nodiscard]] ScenarioText read_scenario_text() noexcept
{
    const std::uint32_t size = file_size_bytes(scenario_file);

    if (size >= scenario_text.size())
    {
        static_cast<void>(f_close(&scenario_file));
        return {.error = EngineError::scenario_too_large};
    }

    UINT bytes_read{};
    const FRESULT read = f_read(&scenario_file, scenario_text.data(), size, &bytes_read);
    static_cast<void>(f_close(&scenario_file));

    if (read != FR_OK || bytes_read != size)
    {
        return {.error = EngineError::scenario_read};
    }

    std::span{scenario_text}[bytes_read] = '\0';
    return {.text = std::string_view{scenario_text.data(), bytes_read}};
}

// zlib's CRC-32 of a text's bytes.
[[nodiscard]] std::uint32_t text_crc32(std::string_view text) noexcept
{
    Crc32 crc;

    for (const char character : text)
    {
        crc.update(std::array{std::bit_cast<std::uint8_t>(character)});
    }

    return crc.value();
}

// What loading a scenario file found: whether the file exists, why it was
// refused (the engine_error, and the Status that said why: the parser's or
// the engine's for scenario_parse and configure_failed, the error's own
// otherwise), and, for a scenario it configured, the CRC-32 of its bytes (a
// PLAYLIST item's block keeps it; a refusal has none, and its callers read
// only the error).
struct ScenarioLoad
{
    bool present{};
    bool cancelled{};
    bool contains_second_source{};
    EngineError error{EngineError::none};
    std::uint32_t crc32{};
    SecondarySourceIdentity secondary_source{};
    m110::Status reason{};
    bool cleanup_failed{};
};

// A refusal whose reason is the engine error itself.
[[nodiscard]] constexpr ScenarioLoad refused_load(EngineError error) noexcept
{
    return {.present = true, .error = error, .reason = status_of(error)};
}

// Destroy stages before closing their borrowed source, then close the source
// while the player task still owns the mounted volume.
[[nodiscard]] m110::Status detach_secondary_source() noexcept
{
    engine().configure_passthrough();
    engine_active = false;
    const auto closed = player_secondary_source().close();

    if (!closed.is_ok())
    {
        player_cleanup_pending = true;
    }

    return closed;
}

// A failed close keeps its FIL and mount owned. STOP retries this cleanup;
// nothing else may reconfigure the engine or hand the volume to the host.
[[nodiscard]] m110::Status retry_player_cleanup() noexcept
{
    if (const auto closed = detach_secondary_source(); !closed.is_ok())
    {
        return closed;
    }

    if (primary_close_pending)
    {
        if (f_close(&playback_file) != FR_OK)
        {
            return {.code = m110::StatusCode::io_error, .message = "PLAYER_CLOSE"};
        }

        primary_close_pending = false;
    }

    if (f_mount(nullptr, drive_path, 0U) != FR_OK)
    {
        player_cleanup_pending = true;
        return {.code = m110::StatusCode::io_error, .message = "PLAYER_UNMOUNT"};
    }

    const TaskCriticalSection lock;
    player_cleanup_pending = false;
    file_system_owned = false;
    run_busy = false;
    return m110::Status::success();
}

// Open and measure the configured excerpt in bounded blocks. The caller
// services STOP between each FatFs transaction; no provider callback or
// cancellation function runs on the signal-processing path.
struct SecondarySourceLoad
{
    SecondarySourceIdentity identity{};
    m110::Status status{};
    bool cancelled{};
};

[[nodiscard]] SecondarySourceLoad measure_secondary_source() noexcept
{
    if (const m110::Status opened = player_secondary_source().open(scenario.second_source_path, scenario.second_source); !opened.is_ok())
    {
        return {.status = opened};
    }

    for (;;)
    {
        poll_secondary_source_command();

        if (stop_requested || (run_busy && faulted))
        {
            return {.cancelled = true};
        }

        const auto measured = player_secondary_source().measure_step(std::span{engine_input}.first(signal_lab::engine_block_frames));

        if (!measured.has_value())
        {
            return {.status = measured.status()};
        }

        if (measured.value() == SecondaryMeasureProgress::complete)
        {
            poll_secondary_source_command();

            if (stop_requested || (run_busy && faulted))
            {
                return {.cancelled = true};
            }

            const auto digest = player_secondary_source().measured_digest();
            constexpr auto word_bits = std::numeric_limits<std::uint32_t>::digits;
            constexpr auto word_mask = std::numeric_limits<std::uint32_t>::max();
            static_assert(signal_lab::second_source_algorithm_version <= std::numeric_limits<std::uint32_t>::max());
            return {
                .identity =
                    {
                        .version = static_cast<std::uint32_t>(signal_lab::second_source_algorithm_version),
                        .source_frames = static_cast<std::uint32_t>(player_secondary_source().frames()),
                        .excerpt_start_frame = static_cast<std::uint32_t>(player_secondary_source().selected_start()),
                        .excerpt_frames = static_cast<std::uint32_t>(player_secondary_source().selected_frames()),
                        .digest_hi = static_cast<std::uint32_t>(digest >> word_bits),
                        .digest_lo = static_cast<std::uint32_t>(digest & word_mask),
                        .latency_frames = static_cast<std::uint32_t>(signal_lab::second_source_filter_latency_frames(scenario.second_source).value()),
                        .reference_rms = player_secondary_source().reference_rms(),
                    },
            };
        }
    }
}

// Playlist preflight checks a second-source scenario without scanning the
// whole excerpt. The provider has already validated the WAV and source range;
// temporarily omit only that stage so the engine can check the remaining
// stages without inventing a source RMS or identity.
// Keep this separate from parse_scenario's large JSON frame so its saved
// stage scratch does not overlap that call's peak stack use.
[[gnu::noinline, nodiscard]] m110::Status configure_preflight_without_second_source(std::uint32_t file_frames) noexcept
{
    std::uint32_t source_index = 0U;

    while (source_index < scenario.stage_count && scenario.stages[source_index].type != signal_lab::StageType::second_source)
    {
        ++source_index;
    }

    if (source_index == scenario.stage_count)
    {
        return m110::Status::success();
    }

    const signal_lab::Stage removed_stage = scenario.stages[source_index];

    for (std::uint32_t index = source_index; index + 1U < scenario.stage_count; ++index)
    {
        scenario.stages[index] = scenario.stages[index + 1U];
    }

    --scenario.stage_count;
    const signal_lab::StageResources resources{&player_watterson(), nullptr, nullptr};
    const m110::Status configured = engine().configure(scenario, scenario.reference_rms, file_frames, resources);
    engine().configure_passthrough();
    engine_active = false;

    for (std::uint32_t index = scenario.stage_count; index > source_index; --index)
    {
        scenario.stages[index] = scenario.stages[index - 1U];
    }

    scenario.stages[source_index] = removed_stage;
    ++scenario.stage_count;
    return configured;
}

[[nodiscard]] ScenarioLoad preflight_second_source(std::uint32_t file_frames, std::uint32_t crc32) noexcept
{
    poll_secondary_source_command();

    if (stop_requested)
    {
        return {.present = true, .cancelled = true, .contains_second_source = true};
    }

    if (const m110::Status opened_source = player_secondary_source().open(scenario.second_source_path, scenario.second_source); !opened_source.is_ok())
    {
        const m110::Status closed = detach_secondary_source();
        return {.present = true,
                .contains_second_source = true,
                .error = EngineError::configure_failed,
                .reason = closed.is_ok() ? opened_source : closed,
                .cleanup_failed = !closed.is_ok() || opened_source.message == secondary_wav_close_failed.message};
    }

    const m110::Status configured = configure_preflight_without_second_source(file_frames);
    const m110::Status closed = detach_secondary_source();
    poll_secondary_source_command();

    if (!closed.is_ok())
    {
        return {.present = true, .contains_second_source = true, .error = EngineError::configure_failed, .reason = closed, .cleanup_failed = true};
    }

    if (stop_requested)
    {
        return {.present = true, .cancelled = true, .contains_second_source = true};
    }

    if (!configured.is_ok())
    {
        return {.present = true, .contains_second_source = true, .error = EngineError::configure_failed, .reason = configured};
    }

    return {.present = true, .contains_second_source = true, .crc32 = crc32};
}

// The scenario at `path`, read, parsed and configured into the engine for a
// file of `file_frames` frames, as PLAY loads one; `seed`, a PLAYLIST item's,
// replaces the scenario's own. PLAY and the PLAYLIST card checks share it.
// An empty path, a PLAYLIST item's without scn=, names no scenario.
[[nodiscard]] ScenarioLoad configure_scenario(const char* path, std::uint32_t file_frames, std::optional<std::uint32_t> seed) noexcept
{
    if (const m110::Status closed = detach_secondary_source(); !closed.is_ok())
    {
        return {.present = true, .error = EngineError::configure_failed, .reason = closed, .cleanup_failed = true};
    }

    if (std::string_view{path}.empty())
    {
        return {};
    }

    const FRESULT opened = f_open(&scenario_file, path, FA_READ);

    if (opened == FR_NO_FILE || opened == FR_NO_PATH)
    {
        return {};
    }

    if (opened != FR_OK)
    {
        return refused_load(EngineError::scenario_open);
    }

    const auto read = read_scenario_text();

    if (read.error != EngineError::none)
    {
        return refused_load(read.error);
    }

    const std::uint32_t crc32 = text_crc32(read.text);

    if (const m110::Status parsed = signal_lab::parse_scenario(read.text.data(), read.text.size(), scenario); !parsed.is_ok())
    {
        return {.present = true, .error = EngineError::scenario_parse, .reason = parsed};
    }

    if (!scenario.has_reference_rms)
    {
        return refused_load(EngineError::missing_reference_rms);
    }

    if (seed.has_value())
    {
        scenario.seed = *seed;
    }

    const auto stages = std::span{scenario.stages}.first(scenario.stage_count);
    const bool has_second_source = std::ranges::any_of(stages, [](const signal_lab::Stage& stage) { return stage.type == signal_lab::StageType::second_source; });

    SecondarySourceIdentity secondary_identity{};

    if (has_second_source)
    {
        if (playlist_preflight_active)
        {
            return preflight_second_source(file_frames, crc32);
        }

        const SecondarySourceLoad measured = measure_secondary_source();

        if (measured.cancelled)
        {
            if (const m110::Status closed = detach_secondary_source(); !closed.is_ok())
            {
                return {.present = true, .contains_second_source = true, .error = EngineError::configure_failed, .reason = closed, .cleanup_failed = true};
            }

            return {.present = true, .cancelled = true, .contains_second_source = true};
        }

        if (!measured.status.is_ok())
        {
            const m110::Status closed = detach_secondary_source();
            return {.present = true, .contains_second_source = true, .error = EngineError::configure_failed, .reason = closed.is_ok() ? measured.status : closed, .cleanup_failed = !closed.is_ok()};
        }

        secondary_identity = measured.identity;
    }

    const signal_lab::StageResources resources{&player_watterson(), nullptr, has_second_source ? &player_secondary_source() : nullptr};

    if (const m110::Status configured = engine().configure(scenario, scenario.reference_rms, file_frames, resources); !configured.is_ok())
    {
        const m110::Status closed = detach_secondary_source();
        return {
            .present = true, .contains_second_source = has_second_source, .error = EngineError::configure_failed, .reason = closed.is_ok() ? configured : closed, .cleanup_failed = !closed.is_ok()};
    }

    return {.present = true, .contains_second_source = has_second_source, .crc32 = crc32, .secondary_source = secondary_identity};
}

// Copy a successfully configured channel's identity; never borrow its report
// across a later preflight/configure, which reuses the channel storage.
[[nodiscard]] playlist::WattersonIdentity configured_watterson() noexcept
{
    const auto* report = engine().watterson_report();

    if (report == nullptr)
    {
        return {};
    }

    static_assert(signal_lab::watterson_algorithm_version <= std::numeric_limits<std::uint32_t>::max());
    constexpr auto word_bits = std::numeric_limits<std::uint32_t>::digits;
    constexpr auto word_mask = std::numeric_limits<std::uint32_t>::max();
    return {.version = static_cast<std::uint32_t>(signal_lab::watterson_algorithm_version),
            .digest_hi = static_cast<std::uint32_t>(report->parameter_digest >> word_bits),
            .digest_lo = static_cast<std::uint32_t>(report->parameter_digest & word_mask),
            .latency_frames = static_cast<std::uint32_t>(report->latency.value()),
            .paths = report->path_count};
}

// Load 2:/WG/PLAY.SCN if present and configure the engine for the play's
// `file_frames` input frames: this WAV's, or a looped item's duration. A missing
// scenario means clean pass-through; a rejected one is reported through STATUS
// (engine_state=2, engine_error) and playback continues unimpaired so the card
// never becomes unplayable because of a bad document. A PLAYLIST item plays
// the scenario its scn= names (in scenario_path) with its seed, or none: its
// path is then empty (select_item_scenario()).
void load_scenario(std::uint32_t file_frames) noexcept
{
    engine_active = false;
    const auto loaded = configure_scenario(std::data(scenario_path), file_frames, player_playlist().scenario_seed);

    if (loaded.cancelled)
    {
        return;
    }

    if (!loaded.present)
    {
        return;
    }

    if (loaded.error != EngineError::none)
    {
        set_engine_error(loaded.error, loaded.reason);

        if (loaded.contains_second_source && playlist_owns_player())
        {
            latch_fault(PlayerError::impairment);
        }

        return;
    }

    taskENTER_CRITICAL();
    engine_counters.state = std::to_underlying(EngineState::active);
    engine_counters.error = std::to_underlying(EngineError::none);
    engine_counters.stages = engine().stats().stage_count;
    engine_counters.arena_bytes = static_cast<std::uint32_t>(engine().stats().arena_bytes_used);
    play_watch().watterson = configured_watterson();
    play_watch().secondary_source = loaded.secondary_source;
    taskEXIT_CRITICAL();
    engine_active = true;
}

void stop_codec_after_fault() noexcept
{
    auto& codec = m110::imxrt1170::wm8960_codec();

    if (codec.is_running())
    {
        (void)codec.stop();
    }
}

// Waits, at most miss_settle_ms, for the completion after the gap that
// last added to the USB play's missed intervals: a late read of its last
// file frames is then taken back or confirmed (usb_audio::confirmed_missed())
// before STATUS or its block reads the count. A host that has stopped
// polling leaves the count as it stands.
void settle_play_misses() noexcept
{
    for (std::uint32_t waited = 0U; waited < miss_settle_ms && usb_play_statistics().miss_unconfirmed; ++waited)
    {
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1));
    }
}

// Waits for the disarmed USB play's queued file frames to be confirmed sent
// (a closed stream has none). It ends with usb_stall once no IN completion has
// come for more than usb_stall_timeout while frames are queued: a host that
// stopped polling, after STOP as during an armed play; the frames may still
// go out later. It ends with usb_unconfirmed once they have not fallen for as
// long while completions still come (QueuedFramesWatch). An earlier fault
// keeps its own error (latch_fault()).
void wait_for_queued_frames() noexcept
{
    QueuedFramesWatch watch{};

    for (;;)
    {
        const auto play = usb_play_statistics();
        const auto now = current_tick();

        if (play.pending_frames == usb_audio::FrameCount{})
        {
            return;
        }

        if (usb_stalled(play, now))
        {
            latch_fault(PlayerError::usb_stall);
            return;
        }

        if (watch.stuck(play, now))
        {
            latch_fault(PlayerError::usb_unconfirmed);
            return;
        }

        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1));
    }
}

// A USB play during which a completion reported a size other than the packet
// TinyUSB had loaded, while the play had file frames queued
// (usb_audio::play_mismatched()), ends with usb_unconfirmed unless an earlier
// fault is its error. Its statistics count that packet sent, as TinyUSB took
// it from the FIFO, but the host may not have received it (the ChipIdea
// driver reports a transfer that ended in error as its expected minus
// remaining bytes, whether its data went out or not), so they are no proof of
// what the host has. The play has run to its end, so the host's capture
// shows which it was. A looped item ends at the mismatch (looped_anomaly()).
void judge_usb_delivery() noexcept
{
    if (usb_play_mismatched())
    {
        latch_fault(PlayerError::usb_unconfirmed);
    }
}

// Ends the output at the end of every run. The codec stops at once. The USB
// sink returns to silence, then waits until the chunks it already queued are
// confirmed sent, so the play statistics are final when the run reports its
// end; a closed stream drops them. A host that has not polled for
// usb_stall_timeout_ms ends the wait at once (error 20), and so do queued
// frames that have not fallen for as long while completions still come
// (error 26): the chunks still queued (at most the two in the FIFO and the
// one loaded) stay queued, and if they go out later they still count. Once
// their missed intervals have settled (settle_play_misses()), the statistics
// describe every packet TinyUSB sent, which is what the host received unless
// a completion did not confirm its packet: then the run ends with error 26
// (judge_usb_delivery()).
void stop_output() noexcept
{
    if (!usb_route())
    {
        stop_codec_after_fault();
        return;
    }

    {
        const TaskCriticalSection lock;
        usb_audio::disarm_play();
    }

    wait_for_queued_frames();
    settle_play_misses();
    judge_usb_delivery();
}

bool stopped_state() noexcept
{
    return !run_busy && !file_system_owned && !play_requested && !preparation_busy;
}

// The route an OUTPUT command names; none for every other command.
[[nodiscard]] std::optional<OutputRoute> requested_output_route(live_protocol::Kind kind) noexcept
{
    if (kind == live_protocol::Kind::output_codec)
    {
        return OutputRoute::codec;
    }

    if (kind == live_protocol::Kind::output_usb)
    {
        return OutputRoute::usb;
    }

    return std::nullopt;
}

// A change needs a stopped player, as LOAD does.
m110::Status OutputRouteSelector::select(OutputRoute requested) noexcept
{
    if (requested != route_.load() && !stopped_state())
    {
        return refusal::busy;
    }

    route_.store(requested);
    return m110::Status::success();
}

void reset_selection(const char* name) noexcept
{
    char* const name_start = std::ranges::copy(media_directory, std::begin(playback_path)).out;
    std::strcpy(name_start, name);
    std::strcpy(scenario_path, playback_path);
    std::strcpy(std::strrchr(scenario_path, '.'), ".SCN");
    reference_override = 0.0;
    live_reference_valid = false;
    (void)live_controller.reset(live_seed, 1.0);
    taskENTER_CRITICAL();
    std::strcpy(live_snapshot.selected_file, name);
    player_state = PlayerState::waiting_for_command;
    taskEXIT_CRITICAL();
}

// CUT (a stopped player): `cuts` become the list the next PLAY takes, once
// cut::validate() accepts them; none empties it. A refused list changes
// nothing and its code is the reply's. The reply's events are the entries
// the next PLAY will take.
void accept_cuts(const cut::CutList& cuts, ControlReply& reply) noexcept
{
    const auto valid = cut::validate(cuts);

    if (!valid.has_value())
    {
        refuse(reply, valid.status());
        return;
    }

    player_cuts().pending = valid.value();
    reply.ok = true;
    reply.events = valid.value().size();
}

// An accepted PLAY takes the pending list into the cut stage, which starts
// with no output, the list empties, and the play's log and route restart.
// Caller: the PLAY's critical section, with the run's counters.
void latch_cuts() noexcept
{
    auto& cuts = player_cuts();
    cuts.stage.latch(cuts.pending);
    cuts.pending.clear();
    cuts.log.restart();
    cuts.route = output_route.current();
}

// A live control: applied by the live controller while a run plays, or queued
// before PLAY; BAD_STATE otherwise, and BUSY while a playlist holds the
// player, whose items run exactly as their lines say.
void apply_live_command(const live_protocol::Request& command, ControlReply& reply) noexcept
{
    if (playlist_owns_player())
    {
        refuse(reply, refusal::busy);
        return;
    }

    if ((!run_busy && player_state != PlayerState::waiting_for_command) || (run_busy && player_state != PlayerState::buffering && player_state != PlayerState::playing) ||
        (run_busy && (eof_enqueued || stop_requested || !live_reference_valid)))
    {
        refuse(reply, refusal::bad_state);
        return;
    }

    if (const m110::Status refused = live_protocol::apply_control(command, live_controller, reply.apply_frame, reply.events); !refused.is_ok())
    {
        refuse(reply, refused);
        return;
    }

    reply.ok = true;
}

// Only the player task mutates the preparation job. Queries read this copy.
void publish_preparation() noexcept
{
    const TaskCriticalSection lock;
    preparation_snapshot = preparation_job->snapshot();

    if (prepared_operation)
    {
        prepared_playback_diagnostics.progress = prepared_playback.snapshot();
        prepared_playback_diagnostics.timing = prepared_playback.timing_snapshot();
        prepared_playback_diagnostics.active = prepared_playback.active();
        prepared_playback_diagnostics.status = prepared_playback.status();
    }

    if (prepared_operation && prepared_failed && !prepared_playback.status().is_ok() && preparation_snapshot.phase == prepare::Phase::cancelled)
    {
        preparation_snapshot.phase = prepare::Phase::failed;
        preparation_snapshot.status = prepared_playback.status();
    }

    preparation_busy = preparation_job->busy() || prepared_playback.active();
    file_system_owned = preparation_busy.load();
}

void service_preparation_playback() noexcept;

void claim_preparation(const prepare::Request& request) noexcept
{
    const TaskCriticalSection lock;
    preparation_snapshot = {.phase = prepare::Phase::starting, .request = request};
    preparation_busy = true;
    file_system_owned = true;
}

void refuse_preparation_claim() noexcept
{
    const TaskCriticalSection lock;
    preparation_snapshot.phase = prepare::Phase::failed;
    preparation_snapshot.status = refusal::bad_state;
    preparation_busy = false;
    file_system_owned = false;
}

void service_preparation() noexcept
{
    if (prepared_playback.active())
    {
        service_preparation_playback();
        return;
    }

    if (!preparation_busy)
    {
        return;
    }

    // step() latches every failure in its snapshot and performs bounded
    // cleanup. A failed cleanup retains ownership until STOP retries it.
    const auto advanced = preparation_job->step();
    static_cast<void>(advanced);
    publish_preparation();
    publish_live();
}

void accept_preparation(const prepare::Request& request, ControlReply& reply) noexcept
{
    if (tx_artifact_busy())
    {
        refuse(reply, refusal::busy);
        return;
    }

    // Claim before the locality check so an asynchronous host-media request
    // cannot pass the ownership guard between admission and the first step.
    prepared_operation = false;
    claim_preparation(request);

    if (!WFG_MediaIsLocal())
    {
        refuse_preparation_claim();
        refuse(reply, refusal::bad_state);
        return;
    }

    const PrepareJob::Resources resources{&engine(), &player_watterson(), &player_secondary_source(), &scenario, engine_input, engine_output, scenario_text, prepare_packed, read_buffer};
    const auto admitted = preparation_job->begin(request, resources);
    publish_preparation();

    if (!admitted.is_ok())
    {
        refuse(reply, admitted);
        return;
    }

    reply.ok = true;
}

// STOP ends the run, and a running playlist with it, in its gaps and pauses
// too; with neither, it cancels preparation or aborts file generation.
void stop_command(ControlReply& reply) noexcept
{
    if (prepared_playback.active())
    {
        stop_requested = true;
        const auto stopped = prepared_playback.cancel();

        if (!prepared_output_stopped)
        {
            stop_output();
            prepared_output_stopped = true;
        }

        publish_preparation();
        reply.ok = stopped.is_ok();

        if (!stopped.is_ok())
        {
            refuse(reply, stopped);
        }

        return;
    }

    if (preparation_busy)
    {
        const auto stopped = preparation_job->cancel();
        publish_preparation();

        if (!stopped.is_ok())
        {
            refuse(reply, stopped);
            return;
        }

        reply.ok = true;
        return;
    }

    if (player_cleanup_pending)
    {
        const auto cleaned = retry_player_cleanup();

        if (!cleaned.is_ok())
        {
            refuse(reply, cleaned);
            return;
        }

        reply.ok = true;
        return;
    }

    const bool playlist_running = playlist_owns_player();

    if (playlist_running)
    {
        player_playlist().stop_requested = true;
    }

    if (run_busy || playlist_preflight_active)
    {
        stop_requested = true;
    }
    else if (!playlist_running)
    {
        abort_tx_artifact();
    }

    reply.ok = true;
}

[[nodiscard]] m110::Result<std::uint32_t> acknowledge_locked(std::uint32_t through) noexcept
{
    const TaskCriticalSection lock;
    return player_playlist().results.acknowledge(through);
}

// PLAYLIST ACK:<seq>, at any time: frees the result blocks numbered `through`
// and below; the reply's events are how many. A pause for a full ring ends
// with it (wait_for_result_room()).
void acknowledge_results(std::uint32_t through, ControlReply& reply) noexcept
{
    const auto freed = acknowledge_locked(through);

    if (!freed.has_value())
    {
        refuse(reply, freed.status());
        return;
    }

    reply.ok = true;
    reply.events = freed.value();
}

// OUTPUT CODEC, OUTPUT USB.
void select_route(OutputRoute route, ControlReply& reply) noexcept
{
    const auto selected = output_route.select(route);

    if (!selected.is_ok())
    {
        refuse(reply, selected);
        return;
    }

    reply.ok = true;
}

// SEED or REFERENCE before PLAY. Once controls are queued it would silently
// erase them: LOAD_REQUIRED.
void configure_live(const live_protocol::Request& command, ControlReply& reply) noexcept
{
    if (live_controller.capture_count() != 0)
    {
        refuse(reply, refusal::load_required);
        return;
    }

    if (command.kind == live_protocol::Kind::seed)
    {
        live_seed = command.integer;
    }
    else
    {
        reference_override = command.value;
    }

    static_cast<void>(live_controller.reset(live_seed, reference_override > 0 ? reference_override : 1.0));

    reply.ok = true;
}

using RunId = std::span<const char, sizeof(LiveSnapshot::run_id)>;
static_assert(sizeof(live_protocol::Request::run_id) == sizeof(LiveSnapshot::run_id), "a PLAY's run ID is the snapshot's");
// A PLAYLIST item's run: no run ID of its own, so the run counter's.
constexpr std::array<char, sizeof(LiveSnapshot::run_id)> no_run_id{};

// The run ID of a PLAY without one: the boot's run counter as 32 lowercase
// hex digits, zero-padded.
void write_counter_run_id(std::uint32_t run, std::span<char, sizeof(LiveSnapshot::run_id)> id) noexcept
{
    constexpr std::string_view hex_digits{"0123456789abcdef"};
    constexpr std::uint32_t hex_radix = 16U;
    constexpr std::size_t digit_count = sizeof(LiveSnapshot::run_id) - 1U;
    static_assert(hex_digits.size() == hex_radix);
    std::ranges::fill(id.first(digit_count), '0');
    id.back() = '\0';
    std::uint32_t rest = run;

    for (std::size_t place = digit_count; place != 0U && rest != 0U; --place)
    {
        id[place - 1U] = hex_digits[rest % hex_radix];
        rest /= hex_radix;
    }
}

// The PLAY critical section, for PLAY and for each PLAYLIST item: the run's
// counters, the output FIFO and its flags restart, the run takes the pending
// CUT list (latch_cuts()), its watch restarts (looped for an item with dur=,
// from the TinyUSB assertion count now), and its run ID is `run_id`, or the
// run counter's when that is empty.
void begin_run(RunId run_id) noexcept
{
    const TaskCriticalSection lock;
    counters = RuntimeCounters{};
    engine_counters = EngineCounters{};
    ring_write_total = 0;
    ring_read_total = 0;
    eof_enqueued = false;
    faulted = false;
    stop_requested = false;
    run_busy = true;
    play_requested = true;
    ++run_number;
    latch_cuts();
    play_watch() = PlayWatch{.asserts_at_start = usb_audio::stack_asserts().count, .looped = player_playlist().duration.has_value()};

    if (run_id.front() != '\0')
    {
        std::ranges::copy(run_id, std::begin(live_snapshot.run_id));
    }
    else
    {
        write_counter_run_id(run_number, live_snapshot.run_id);
    }

    player_state = PlayerState::mounting;
}

// PLAY: a selected file (LOAD, or the run before) on a stopped player, the
// card Local and no file generation (else BAD_STATE), a run ID other than the
// last run's (else RUN_ID_REUSED), and on the USB route an open capture
// stream (accept_output(), else NO_STREAM).
void accept_play_command(const live_protocol::Request& command, ControlReply& reply) noexcept
{
    const RunId run_id{command.run_id};

    if (player_state != PlayerState::waiting_for_command || !WFG_MediaIsLocal() || tx_artifact_busy())
    {
        refuse(reply, refusal::bad_state);
        return;
    }

    if (run_id.front() != '\0' && std::ranges::equal(run_id, std::span{live_snapshot.run_id}))
    {
        refuse(reply, refusal::run_id_reused);
        return;
    }

    if (const auto output = accept_output(); !output.is_ok())
    {
        refuse(reply, output);
        return;
    }

    begin_run(run_id);
    reply.ok = true;
}

// Experimental finite, fixed-length preparation/playback. CUT and queued live
// controls have no meaning on already-perturbed PCM and must not be discarded.
void accept_prepared_playback(const live_protocol::Request& command, ControlReply& reply) noexcept
{
    if (tx_artifact_busy() || player_cuts().pending.size() != 0U || live_controller.capture_count() != 0U)
    {
        refuse(reply, refusal::bad_state);
        return;
    }

    if (command.preparation_rate == 0U)
    {
        refuse(reply, refusal::bad_request);
        return;
    }

    bool operation_id_exhausted{};
    {
        const TaskCriticalSection lock;
        operation_id_exhausted = prepared_playback_operation_id == std::numeric_limits<std::uint64_t>::max();
    }

    if (operation_id_exhausted)
    {
        refuse(reply, refusal::bad_state);
        return;
    }

    if (const auto output = accept_output(); !output.is_ok())
    {
        refuse(reply, output);
        return;
    }

    prepared_operation = false;
    claim_preparation(command.preparation);

    if (!WFG_MediaIsLocal())
    {
        refuse_preparation_claim();
        refuse(reply, refusal::bad_state);
        return;
    }

    const PrepareJob::Resources resources{&engine(), &player_watterson(), &player_secondary_source(), &scenario, engine_input, engine_output, scenario_text, prepare_packed, read_buffer};
    const auto admitted = preparation_job->begin_managed(command.preparation, resources);
    publish_preparation();

    if (!admitted.is_ok())
    {
        refuse(reply, admitted);
        return;
    }

    const auto configured = prepared_playback.begin(*preparation_job, {.measured_rate_frames_per_second = command.preparation_rate,
                                                                       .margin_frames = command.preparation_margin_frames,
                                                                       .queue_capacity_frames = ring_high_watermark_frames,
                                                                       .max_block_frames = read_block_frames});

    if (!configured.is_ok())
    {
        static_cast<void>(preparation_job->cancel());
        refuse(reply, configured);
        return;
    }

    begin_run(RunId{command.run_id});
    prepared_operation = true;
    prepared_failed = false;
    {
        const TaskCriticalSection lock;
        ++prepared_playback_operation_id;
        prepared_playback_diagnostics.operation_id = prepared_playback_operation_id;
        prepared_playback_diagnostics.measured_rate_frames_per_second = command.preparation_rate;
        prepared_playback_diagnostics.margin_frames = command.preparation_margin_frames;
        publish_preparation();
    }

    play_requested = false;
    live_reference_valid = false;
    prepared_consumed_frames = 0U;
    prepared_output_stopped = false;
    prepared_drain = QueuedFramesWatch{};
    set_state(PlayerState::buffering);
    reply.ok = true;
}

// A request's NUL-terminated text field.
[[nodiscard]] std::string_view request_text(std::span<const char> field) noexcept
{
    return {field.begin(), std::ranges::find(field, '\0')};
}

// 2:/WG/<name>, as playback_path holds a file's path.
[[nodiscard]] MediaPath media_path(std::string_view name) noexcept
{
    MediaPath path{};
    const auto name_start = std::ranges::copy(media_directory, path.begin()).out;
    std::ranges::copy(name.substr(0U, path.size() - media_directory.size() - 1U), name_start);
    return path;
}

// A PLAYLIST item's scenario: the file its scn= names, 2:/WG/<name>, or none
// without scn= (an empty path), never its WAV's own <FILE>.SCN that LOAD
// selects. An item plays what its line says, and its block's scn and
// scn_crc32 say which scenario it played.
void select_item_scenario(const playlist::ShortName& name) noexcept
{
    const MediaPath none{};
    std::ranges::copy(name.empty() ? none : media_path(name.view()), std::begin(scenario_path));
}

// A PLAYLIST start's file or card check: its status, the playlist file's line
// it names (0 for the file as a whole), and what the check found there.
struct PlaylistCheck
{
    m110::Status status{};
    playlist::LineNumber line{};
    const char* detail{};
    bool cleanup_failed{};

    [[nodiscard]] bool is_ok() const noexcept { return status.is_ok(); }
};

[[nodiscard]] m110::Status close_preflight_playback_file() noexcept
{
    if (f_close(&playback_file) == FR_OK)
    {
        return m110::Status::success();
    }

    primary_close_pending = true;
    player_cleanup_pending = true;
    return {.code = m110::StatusCode::io_error, .message = "PLAYER_CLOSE"};
}

// The open playback_file's bytes through the parser into `table`, a read
// buffer at a time.
[[nodiscard]] PlaylistCheck parse_playlist_file(playlist::ItemTable& table) noexcept
{
    const std::uint32_t size = file_size_bytes(playback_file);

    if (size > playlist::max_file_bytes)
    {
        return {.status = playlist::playlist_too_large};
    }

    playlist::FileParser parser{};

    for (std::uint32_t done = 0U; done < size;)
    {
        const UINT requested = std::min<UINT>(read_buffer_bytes, size - done);
        const auto chunk = std::span{read_buffer}.first(requested);
        UINT actual{};

        if (f_read(&playback_file, chunk.data(), requested, &actual) != FR_OK || actual != requested)
        {
            return {.status = playlist::playlist_unreadable, .detail = "read"};
        }

        const auto fed = parser.feed(chunk, table, offered_routes);

        if (!fed.is_ok())
        {
            return {.status = fed, .line = parser.line()};
        }

        done += actual;
    }

    const auto finished = parser.finish(table, offered_routes);
    return {.status = finished, .line = parser.line()};
}

// The playlist file 2:/WG/<name>, parsed into the item table, and closed.
[[nodiscard]] PlaylistCheck read_playlist_file(const playlist::ShortName& name) noexcept
{
    auto& table = player_playlist().table;
    table.clear();

    if (f_open(&playback_file, media_path(name.view()).data(), FA_READ) != FR_OK)
    {
        return {.status = playlist::playlist_unreadable};
    }

    const auto checked = parse_playlist_file(table);

    if (const m110::Status closed = close_preflight_playback_file(); !closed.is_ok())
    {
        return {.status = closed, .detail = "close", .cleanup_failed = true};
    }

    return checked;
}

// One item's card checks: its WAV opens and passes PLAY's checks, and its
// scenario (scn=) loads as PLAY loads one, for the item's input (the WAV's
// length, or its dur=) and with the item's seed. The scenario's CRC-32 is
// kept for the item's result block.
[[nodiscard]] PlaylistCheck check_item(playlist::Item& item) noexcept
{
    if (const m110::Status detached = detach_secondary_source(); !detached.is_ok())
    {
        return {.status = playlist::scenario_invalid, .line = item.line, .detail = error_name(EngineError::configure_failed), .cleanup_failed = true};
    }

    if (f_open(&playback_file, media_path(item.file.view()).data(), FA_READ) != FR_OK)
    {
        return {.status = playlist::wav_missing, .line = item.line};
    }

    WavPayload payload{};
    const auto wav = validate_open_wav(payload, ReadCounting::uncounted);

    if (const m110::Status closed = close_preflight_playback_file(); !closed.is_ok())
    {
        return {.status = closed, .line = item.line, .detail = "close", .cleanup_failed = true};
    }

    if (wav != WavValidationError::none)
    {
        return {.status = playlist::wav_invalid, .line = item.line, .detail = error_name(player_error_of(wav))};
    }

    if (item.scenario.empty())
    {
        return {};
    }

    const auto input_frames = play_input_frames(payload.bytes / audio::pcm24_bytes_per_sample, item.duration);
    const auto loaded = configure_scenario(media_path(item.scenario.view()).data(), input_frames, item.seed);
    const m110::Status detached = detach_secondary_source();

    if (loaded.cleanup_failed || !detached.is_ok())
    {
        return {.status = playlist::scenario_invalid, .line = item.line, .detail = error_name(EngineError::configure_failed), .cleanup_failed = true};
    }

    if (loaded.cancelled || stop_requested)
    {
        return {.status = playlist_stopped, .line = item.line};
    }

    if (!loaded.present)
    {
        return {.status = playlist::scenario_missing, .line = item.line};
    }

    if (loaded.error != EngineError::none)
    {
        return {.status = playlist::scenario_invalid, .line = item.line, .detail = error_name(loaded.error)};
    }

    item.scenario_crc32 = loaded.crc32;
    return {};
}

// Every item's card checks, in the file's order: the first failure names its
// line.
[[nodiscard]] PlaylistCheck check_items() noexcept
{
    for (auto& item : player_playlist().table.items())
    {
        const auto checked = check_item(item);

        if (!checked.is_ok())
        {
            return checked;
        }
    }

    return {};
}

// A PLAYLIST start's file and card checks, with the card mounted for them
// and released after them: a refusal leaves the player as it was.
[[nodiscard]] PlaylistCheck load_playlist(const playlist::ShortName& name) noexcept
{
    if (f_mount(&file_system, std::data(drive_path), 1U) != FR_OK)
    {
        return {.status = playlist::playlist_unreadable, .detail = "mount"};
    }

    file_system_owned = true;
    stop_requested = false;
    playlist_preflight_active = true;
    playlist_preflight_reject_pending = false;
    auto checked = read_playlist_file(name);

    if (checked.is_ok())
    {
        checked = check_items();
    }

    if (!player_cleanup_pending)
    {
        if (f_mount(nullptr, std::data(drive_path), 0U) != FR_OK)
        {
            player_cleanup_pending = true;
            checked = {.status = {.code = m110::StatusCode::io_error, .message = "PLAYER_UNMOUNT"}, .cleanup_failed = true};
        }
        else
        {
            file_system_owned = false;
        }
    }

    taskENTER_CRITICAL();

    if (command_pending)
    {
        if (command_mailbox().kind == live_protocol::Kind::stop)
        {
            stop_requested = true;
        }
        else
        {
            playlist_preflight_reject_pending = true;
        }
    }

    playlist_preflight_active = false;
    const bool stopped = stop_requested;
    stop_requested = false;
    taskEXIT_CRITICAL();

    if (stopped && !checked.cleanup_failed)
    {
        return {.status = playlist_stopped};
    }

    return checked;
}

// An accepted PLAYLIST: its progress starts at its first item, and it holds
// the player until it ends; the player task runs it next (run_playlist()).
void begin_playlist(const playlist::ShortName& name, playlist::Policy policy) noexcept
{
    auto& list = player_playlist();
    const TaskCriticalSection lock;
    list.progress = playlist::Progress{.state = playlist::State::running, .name = name, .run = list.progress.run + 1U, .policy = policy, .items = list.table.size()};
    list.owns_player = true;
    list.run_requested = true;
    list.stop_requested = false;
}

[[nodiscard]] m110::Status unmount_card_listing() noexcept
{
    if (f_mount(nullptr, std::data(drive_path), 0U) != FR_OK)
    {
        card_listing_cleanup_pending = true;
        file_system_owned = true;
        return refusal::files_unmount;
    }

    card_listing_cleanup_pending = false;
    return m110::Status::success();
}

// FILES? owns the local card only while it enumerates. The reply borrows the
// static OCRAM page; no FatFs handle or mount survives a successful response.
void accept_files(const live_protocol::Request& command, ControlReply& reply) noexcept
{
    if (playlist_owns_player() || run_busy || play_requested || tx_artifact_busy() || (file_system_owned && !card_listing_cleanup_pending))
    {
        refuse(reply, refusal::busy);
        return;
    }

    const bool retry_cleanup = card_listing_cleanup_pending;
    file_system_owned = true;

    if (!WFG_MediaIsLocal())
    {
        if (!retry_cleanup)
        {
            file_system_owned = false;
        }

        refuse(reply, refusal::host_owned);
        return;
    }

    if (retry_cleanup)
    {
        if (const m110::Status unmounted = unmount_card_listing(); !unmounted.is_ok())
        {
            refuse(reply, unmounted);
            return;
        }
    }

    const bool mounted = f_mount(&file_system, std::data(drive_path), 1U) == FR_OK;
    m110::Status result = refusal::files_mount;

    if (mounted)
    {
        result = card_listing::read_page(command.integer, card_listing_page());
    }

    if (const m110::Status unmounted = unmount_card_listing(); !unmounted.is_ok())
    {
        refuse(reply, unmounted);
        return;
    }

    file_system_owned = false;

    if (!result.is_ok())
    {
        refuse(reply, result);
        return;
    }

    reply.files = &card_listing_page();
    reply.ok = true;
}

// PLAYLIST:<NAME>[:CONTINUE] on a stopped player: as PLAY, the card Local
// and no file generation (else BAD_STATE); then every line parses and every
// item passes its card checks, else the first refusal, naming its line. The
// reply's events are the items.
void accept_playlist(const live_protocol::Request& command, ControlReply& reply) noexcept
{
    if (!WFG_MediaIsLocal() || tx_artifact_busy())
    {
        refuse(reply, refusal::bad_state);
        return;
    }

    const auto name = playlist::ShortName::from(request_text(command.name));
    const auto checked = load_playlist(name);

    if (!checked.is_ok())
    {
        refuse(reply, checked.status);
        reply.line = checked.line.value;
        reply.detail = checked.detail;
        return;
    }

    begin_playlist(name, command.policy);
    reply.ok = true;
    reply.events = player_playlist().table.size();
}

// The commands that need a stopped player.
void dispatch_stopped(const live_protocol::Request& command, ControlReply& reply) noexcept
{
    using live_protocol::Kind;

    switch (command.kind)
    {
    case Kind::load:
        reset_selection(std::data(command.name));
        reply.ok = true;
        break;

    case Kind::cut:
        accept_cuts(command.cuts, reply);
        break;

    case Kind::seed:
    case Kind::reference:
        configure_live(command, reply);
        break;

    case Kind::play:
        accept_play_command(command, reply);
        break;

    case Kind::playlist_start:
        accept_playlist(command, reply);
        break;

    case Kind::prepare_file:
        accept_preparation(command.preparation, reply);
        break;

    case Kind::prepare_play:
        accept_prepared_playback(command, reply);
        break;

    default:
        refuse(reply, refusal::bad_request);
        break;
    }
}

// One command from the mailbox, answered in `reply`. While a playlist holds
// the player only STOP and PLAYLIST ACK act, and every other command that
// would change a run, OUTPUT included, is BUSY.
void dispatch_command(const live_protocol::Request& command, ControlReply& reply) noexcept
{
    using live_protocol::Kind;

    if ((playlist_preflight_active || preparation_busy || player_cleanup_pending) && command.kind != Kind::stop)
    {
        refuse(reply, refusal::busy);
        return;
    }

    if (command.kind == Kind::files)
    {
        accept_files(command, reply);
        return;
    }

    const auto route = requested_output_route(command.kind);

    if (live_protocol::is_control(command.kind))
    {
        apply_live_command(command, reply);
    }
    else if (command.kind == Kind::stop)
    {
        stop_command(reply);
    }
    else if (command.kind == Kind::playlist_ack)
    {
        acknowledge_results(command.ack_seq, reply);
    }
    else if (route.has_value() && !playlist_owns_player())
    {
        select_route(*route, reply);
    }
    else if (playlist_owns_player() || !stopped_state())
    {
        refuse(reply, refusal::busy);
    }
    else
    {
        dispatch_stopped(command, reply);
    }
}

void service_player_command() noexcept
{
    service_tx_artifact();
    taskENTER_CRITICAL();

    if (!command_pending || reply_pending)
    {
        taskEXIT_CRITICAL();
        return;
    }

    const auto command = command_mailbox();
    const bool reject_preflight_command = playlist_preflight_reject_pending;
    command_pending = false;
    playlist_preflight_reject_pending = false;
    taskEXIT_CRITICAL();
    ControlReply reply{};
    reply.seq = command.seq;

    if (reject_preflight_command)
    {
        refuse(reply, refusal::busy);
    }
    else
    {
        dispatch_command(command, reply);
    }

    publish_live();
    taskENTER_CRITICAL();
    reply_mailbox() = reply;
    reply_pending = true;
    taskEXIT_CRITICAL();
}

// Secondary-source measurement runs inside command dispatch for PLAY or
// PLAYLIST, so it cannot recursively dispatch a command. Observe STOP and
// defer other queued commands for a BUSY reply after the outer response.
void poll_secondary_source_command() noexcept
{
    taskENTER_CRITICAL();

    if (command_pending)
    {
        if (command_mailbox().kind == live_protocol::Kind::stop)
        {
            stop_requested = true;
        }
        else
        {
            playlist_preflight_reject_pending = true;
        }
    }

    taskEXIT_CRITICAL();
}

// Deterministic fallback level: RMS of the first min(N,48000) clean PCM frames.
// Scenario reference_rms and explicit REFERENCE override this measurement.
double measure_reference(const WavPayload& payload) noexcept
{
    constexpr std::uint32_t one_second_bytes = 48000U * waveform_generator::audio::pcm24_bytes_per_sample;
    std::uint32_t remaining = payload.bytes < one_second_bytes ? payload.bytes : one_second_bytes;
    const std::uint32_t frames = remaining / waveform_generator::audio::pcm24_bytes_per_sample;
    double energy = 0;

    while (remaining && !stop_requested && !faulted)
    {
        service_player_command();
        const UINT requested = remaining < read_buffer_bytes ? remaining : read_buffer_bytes;
        UINT actual{};

        if (f_read(&playback_file, read_buffer.data(), requested, &actual) != FR_OK || actual != requested)
        {
            latch_fault(PlayerError::sd_read);
            return 0;
        }

        for (UINT i = 0; i < actual / waveform_generator::audio::pcm24_bytes_per_sample; ++i)
        {
            const double value = waveform_generator::audio::pcm24_to_float(
                waveform_generator::audio::read_pcm24_le(std::span{read_buffer}.subspan(std::size_t{i} * waveform_generator::audio::pcm24_bytes_per_sample).data()));
            energy += value * value;
        }

        remaining -= actual;
    }

    if (!seek_to(payload.offset).is_ok())
    {
        latch_fault(PlayerError::seek);
    }

    return frames ? std::sqrt(energy / frames) : 0;
}

// The engine failed a block with `failure`: STATUS engine_state failed (3)
// with the engine_error, `failure` kept for ENGINE?, and the run's impairment
// fault.
void fail_engine_block(m110::Status failure) noexcept
{
    const auto reason = signal_lab::same_failure(failure, signal_lab::engine_failure::clipping) ? EngineError::clipping_rejected : EngineError::processing_failed;
    {
        const TaskCriticalSection lock;
        engine_counters.state = std::to_underlying(EngineState::failed);
        engine_counters.error = std::to_underlying(reason);
        play_watch().engine_reason = failure;
    }

    latch_fault(PlayerError::impairment);
}

// The live controls refused the play's reference, a preset control or a
// block, `why` saying so: the run's live_control fault, and `why` kept for
// ENGINE? (player_live_reason()). The first refusal is kept, as the run keeps
// its first fault (latch_fault()), so ENGINE? names the refusal that stopped
// the play. Caller: the player task.
void fail_live_controls(m110::Status why) noexcept
{
    {
        const TaskCriticalSection lock;
        auto& kept = play_watch().live_reason;
        kept = kept.is_ok() ? why : kept;
    }

    latch_fault(PlayerError::live_control);
}

// The run's live controls: reset with the run's seed for `reference` (the
// caller's choice, the play's reference RMS or 1.0 without one), then the
// controls the controller took before PLAY (`presets`) queued again. A
// refused reset stops the run with its Status. A preset refused after the
// reset is the player's own inconsistency: internal_error, with the
// refusal's name as its message; the presets after it are still queued. Each
// refusal goes to fail_live_controls(); the first is returned, success when
// there is none. Caller: the player task.
[[nodiscard]] m110::Status start_live_controls(double reference, std::span<const signal_lab::ControlEvent> presets) noexcept
{
    if (const m110::Status reset = live_controller.reset(live_seed, reference); !reset.is_ok())
    {
        fail_live_controls(reset);
        return reset;
    }

    m110::Status first_refusal{};

    for (const signal_lab::ControlEvent& preset : presets)
    {
        if (const m110::Status queued = live_controller.enqueue(preset); !queued.is_ok())
        {
            const m110::Status refusal{.code = m110::StatusCode::internal_error, .message = queued.message};
            fail_live_controls(refusal);
            first_refusal = first_refusal.is_ok() ? refusal : first_refusal;
        }
    }

    return first_refusal;
}

// The card frames of a read, its `bytes` in read_buffer, as float samples.
void decode_block(std::span<const std::uint8_t> bytes, std::span<float> samples) noexcept
{
    for (std::size_t frame = 0U; frame < samples.size(); ++frame)
    {
        const auto sample = bytes.subspan(frame * audio::pcm24_bytes_per_sample, audio::pcm24_bytes_per_sample);
        samples[frame] = audio::pcm24_to_float(audio::read_pcm24_le(sample.data()));
    }
}

// A block of card samples through the scenario engine, when one is active,
// and the live controls: the samples for the output FIFO, or a fault.
[[nodiscard]] m110::Result<std::span<const float>> process_block(std::span<const float> input) noexcept
{
    std::size_t produced = input.size();

    if (engine_active)
    {
        const auto begin = m110::platform_cycle_counter();
        const m110::Result<std::size_t> processed = engine().process(input, std::span{engine_output});
        publish_engine_counters(m110::platform_cycle_counter() - begin);

        if (!processed.has_value())
        {
            fail_engine_block(processed.error());
            return processed.error();
        }

        produced = processed.value();
    }
    else
    {
        std::ranges::copy(input, engine_output.begin());
    }

    const auto output = std::span{engine_output}.first(std::min(produced, engine_output.size()));

    if (const m110::Status live = live_controller.process(output.data(), output.size()); !live.is_ok())
    {
        fail_live_controls(live);
        return live;
    }

    return std::span<const float>{output};
}

// A block's frame count as the output FIFO counts frames: an engine block's
// output never exceeds engine_capacity_frames (static_assert above).
[[nodiscard]] std::uint32_t fifo_frames(std::span<const float> block) noexcept
{
    return static_cast<std::uint32_t>(std::min(block.size(), signal_lab::engine_capacity_frames));
}

// The producer's play: the card it reads, the engine's output block on hand,
// which the cut stage walks (player_cuts().stage), and the gain of the play's
// attenuation (a PLAYLIST item's att=; 0 dB, which changes no sample,
// otherwise).
struct ProducerState
{
    CardSource card{};
    std::span<const float> block{}; // the engine's last output block, in engine_output
    attenuation::Gain gain{attenuation::Gain::of(attenuation::none)};
};

// The output FIFO's frames so far: where the producer's next frame stands in
// the play's stream, after its cuts.
[[nodiscard]] cut::SentFrames stream_position() noexcept
{
    return cut::SentFrames{.value = ring_write_total.load()};
}

// A cut the stage applied, for the play's log; the USB task reads the log.
// The log holds a cut for each entry a schedule may have and the stage
// applies each entry at most once, so it never refuses one; if it did, the
// play's cut statistics would miss a cut, and the run latches cut_log.
[[nodiscard]] m110::Status log_cut(const std::optional<cut::AppliedCut>& applied) noexcept
{
    if (!applied.has_value())
    {
        return m110::Status::success();
    }

    const auto recorded = [&applied]
    {
        const TaskCriticalSection lock;
        return player_cuts().log.record(*applied);
    }();

    if (!recorded.is_ok())
    {
        latch_fault(PlayerError::cut_log);
    }

    return recorded;
}

// The engine's output has ended (the card is read): the log takes the DROP
// under way, which the end cut short, and the entries at or past the end,
// which never apply.
[[nodiscard]] m110::Status finish_output() noexcept
{
    auto& cuts = player_cuts();
    const auto end = cuts.stage.finish(stream_position());

    if (const auto logged = log_cut(end.clipped); !logged.is_ok())
    {
        return logged;
    }

    const TaskCriticalSection lock;
    cuts.log.reject(end.past_end);
    return m110::Status::success();
}

// An engine deadline miss: a block of the engine's output ready only once
// the output, already running before the stream's end, has drained the
// output FIFO below ring_critical_frames (100 ms), so the producer, which
// keeps it between its wake and high watermarks (300 to 500 ms), came too
// late with the block. Every play counts them (USB? engine_deadline_misses);
// a looped PLAYLIST item ends at the first (deadline_miss).
void note_block_deadline() noexcept
{
    if (player_state != PlayerState::playing || eof_enqueued || ring_fill() >= ring_critical_frames)
    {
        return;
    }

    auto& watch = play_watch();
    {
        const TaskCriticalSection lock;
        ++watch.deadline_misses;
    }

    if (watch.looped)
    {
        latch_fault(PlayerError::deadline_miss);
    }
}

// A pull step: the card's next block (across a looped item's seam, from
// its next pass) through the engine and the live controls, every frame of
// it, and their output to the cut stage; once the play's input is read, the
// end of the engine's output. A block the stage refuses (past 2^32 - 1
// output frames: static_asserts above) latches cut_log.
[[nodiscard]] m110::Status pull_block(ProducerState& producer) noexcept
{
    if (producer.card.frames_left == 0U)
    {
        return finish_output();
    }

    const auto read = read_card_block(producer.card);

    if (!read.has_value())
    {
        return read.status();
    }

    const auto bytes = read.value();
    const auto input = std::span{engine_input}.first(bytes.size() / audio::pcm24_bytes_per_sample);
    decode_block(bytes, input);
    const auto output = process_block(input);

    if (!output.has_value())
    {
        return output.status();
    }

    publish_live();
    note_block_deadline();
    producer.block = output.value();
    const auto offered = player_cuts().stage.offer(cut::OutputFrames{.value = fifo_frames(producer.block)});

    if (!offered.is_ok())
    {
        latch_fault(PlayerError::cut_log);
    }

    return offered;
}

// A send step: `frames` frames of the engine's block on hand, from its frame
// `from`, into the output FIFO, each PCM24 sample attenuated by `gain`
// (common/attenuation.hpp: exact, and the identity at 0 dB). A ZERO's zeros
// need none: zero stays zero.
void send_output(std::span<const float> block, std::uint32_t from, std::uint32_t frames, attenuation::Gain gain) noexcept
{
    const auto samples = block.subspan(from, frames);
    push_block(frames, [samples, gain](std::uint32_t frame) { return attenuation::apply(gain, signal_lab::det::quantize_pcm24(samples[frame])); });
}

// A ZERO's step: exact zero frames into the output FIFO, past the engine and
// the live controls.
void produce_zeros(std::uint32_t frames) noexcept
{
    push_block(frames, [](std::uint32_t) { return audio::Pcm24Sample{}; });
}

// Runs `step`; a send's frames begin at frame `from` of the block on hand. A
// DROP's frames went through the engine and the live controls with the rest
// of their block, and go no further.
[[nodiscard]] m110::Status run_step(const cut::CutStep& step, ProducerState& producer, std::uint32_t from) noexcept
{
    switch (step.kind)
    {
    case cut::StepKind::pull:
        return pull_block(producer);

    case cut::StepKind::send:
        send_output(producer.block, from, step.frames, producer.gain);
        return m110::Status::success();

    case cut::StepKind::zeros:
        produce_zeros(step.frames);
        return m110::Status::success();

    case cut::StepKind::discard:
    case cut::StepKind::end:
        return m110::Status::success();
    }

    return m110::Status::success();
}

// One producer block's cost and the frames it put into the output FIFO.
void count_producer_block(std::uint32_t cycles, std::uint32_t frames) noexcept
{
    const TaskCriticalSection lock;
    counters.producer_active_cycles = counters.producer_active_cycles + cycles;
    counters.producer_frames = counters.producer_frames + frames;
    counters.producer_worst_block_cycles = std::max(counters.producer_worst_block_cycles, cycles);
}

// Carries out one cut stage step as one producer block: the stage takes it,
// logging the cut it applies before any frame after the cut's place reaches
// the FIFO (a ZERO as it begins, a DROP once its frames are discarded), then
// the step runs: the engine's next block, frames of the block on hand or a
// ZERO's zeros into the FIFO, or nothing more for discarded frames.
[[nodiscard]] m110::Status carry_out(const cut::CutStep& step, ProducerState& producer) noexcept
{
    const auto block_begin = m110::platform_cycle_counter();
    const auto before = stream_position();
    auto& stage = player_cuts().stage;
    const auto from = stage.block_position();

    if (const auto logged = log_cut(stage.take(step, before)); !logged.is_ok())
    {
        return logged;
    }

    const auto done = run_step(step, producer, from);

    if (done.is_ok())
    {
        count_producer_block(m110::platform_cycle_counter() - block_begin, stream_position().value - before.value);
    }

    return done;
}

// The producer sleeps while the output FIFO holds its high watermark, for
// about the time it takes to drain to its wake watermark. A control
// notification wakes it early, without making it deadline-bound.
void pace_producer(std::uint32_t fill) noexcept
{
    counters.producer_sleeps = counters.producer_sleeps + 1U;
    const auto surplus = fill - ring_wake_watermark_frames;
    const auto wait_ms = std::max(surplus / frames_per_millisecond, std::uint32_t{1U});
    static_cast<void>(ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait_ms)));
}

// The engine's output is done and every frame of it sent to the FIFO or
// discarded: the stream ends with the frames in the output FIFO (the log
// notes where), and the FIFO holds its last frames.
void end_stream() noexcept
{
    {
        const TaskCriticalSection lock;
        player_cuts().log.end(stream_position());
    }

    __DMB();
    eof_enqueued = true;
    __DMB();
}

// Observe only frames actually removed by the output consumer. A scheduler
// read is a FIFO enqueue, not evidence that USB or the codec delivered it.
void observe_prepared_output() noexcept
{
    if (!prepared_playback.snapshot().started || prepared_output_stopped)
    {
        return;
    }

    const auto consumed = ring_read_total.load();

    while (prepared_consumed_frames != consumed)
    {
        const auto frames = std::min(consumed - prepared_consumed_frames, read_block_frames);
        const auto advanced = prepared_playback.consume(frames);

        if (!advanced.is_ok())
        {
            latch_fault(PlayerError::underrun);
            return;
        }

        prepared_consumed_frames += frames;
    }

    check_output();
    check_drain(prepared_drain);
}

// Copy the already-perturbed packed PCM directly; never run the engine twice.
// The logical queue is smaller than the physical ring, leaving enough room
// for this bounded read. At most two writes span its wrap boundary.
void enqueue_prepared_frames(std::uint32_t frames) noexcept
{
    const auto first = write_ring(frames, [](std::uint32_t frame) { return audio::read_pcm24_le(std::span{read_buffer}.subspan(std::size_t{frame} * audio::pcm24_bytes_per_sample).data()); });

    const auto second =
        write_ring(frames - first, [first](std::uint32_t frame) { return audio::read_pcm24_le(std::span{read_buffer}.subspan(std::size_t{first + frame} * audio::pcm24_bytes_per_sample).data()); });

    if (first + second != frames)
    {
        static_cast<void>(prepared_playback.fail({m110::StatusCode::internal_error, "PREPAREPLAY_FIFO"}));
        latch_fault(PlayerError::underrun);
    }
}

void finish_prepared_playback() noexcept
{
    if (!prepared_output_stopped)
    {
        stop_output();
        prepared_output_stopped = true;
    }

    const TaskCriticalSection lock;
    run_busy = false;

    if (!faulted)
    {
        counters.pcm_drained = stop_requested ? 0U : 1U;
        player_state = stop_requested ? PlayerState::aborted : PlayerState::done;
    }
}

void service_preparation_playback() noexcept
{
    observe_prepared_output();

    if (faulted && prepared_playback.status().is_ok())
    {
        static_cast<void>(prepared_playback.fail(status_of(static_cast<PlayerError>(counters.error))));
    }

    const bool drained = eof_enqueued && ring_fill() == 0U && !pending_output_drain();

    if (!prepared_output_stopped && (drained || faulted || stop_requested))
    {
        stop_output();
        prepared_output_stopped = true;

        if (faulted)
        {
            static_cast<void>(prepared_playback.fail(status_of(static_cast<PlayerError>(counters.error))));
        }
    }

    const auto advanced = prepared_playback.step(read_buffer, drained && prepared_output_stopped);

    if (!advanced.has_value())
    {
        if (!stop_requested)
        {
            prepared_failed = true;
            latch_fault(PlayerError::sd_read);
        }

        if (!prepared_output_stopped)
        {
            stop_output();
            prepared_output_stopped = true;
        }
    }
    else if (advanced.value() != 0U)
    {
        enqueue_prepared_frames(advanced.value());
    }

    const auto snapshot = prepared_playback.snapshot();
    {
        const TaskCriticalSection lock;
        counters.file_frames_total = static_cast<std::uint32_t>(snapshot.total_frames);
    }

    if (snapshot.total_frames != 0U && snapshot.read_frames == snapshot.total_frames && !eof_enqueued)
    {
        end_stream();
    }

    if (prepared_playback.ready_to_start())
    {
        const auto started = prepared_playback.start();

        if (!started.is_ok() || !start_output().is_ok())
        {
            static_cast<void>(prepared_playback.fail(started.is_ok() ? status_of(static_cast<PlayerError>(counters.error)) : started));
        }
    }

    if (!prepared_playback.active())
    {
        finish_prepared_playback();
    }

    publish_preparation();
    publish_live();
}

// The output route's start for one play. A start that fails latches the
// run's fault (start_output()), which ends the producer.
class OutputStart
{
public:
    // Starts the output route unless it has started.
    void ensure() noexcept
    {
        if (!started_)
        {
            started_ = start_output().is_ok();
        }
    }

private:
    bool started_{};
};

// The producer: reads the play's card (`card`: once, or looped for a
// PLAYLIST item's dur=), puts every frame of it through the engine and the
// live controls, and fills the output FIFO from their output through the cut
// stage and the item's attenuation until that output is done, STOP or a
// fault; it starts the output once the FIFO holds its prefill, or the whole
// stream when that is shorter. Each step puts at most output_step_frames
// frames into the FIFO.
void produce(const CardSource& card) noexcept
{
    OutputStart output{};
    ProducerState producer{.card = card, .block = {}, .gain = attenuation::Gain::of(player_playlist().attenuation)};

    while (!faulted && !stop_requested)
    {
        service_player_command();
        check_output();

        if (stop_requested)
        {
            return;
        }

        const auto step = player_cuts().stage.next(output_step);

        if (step.kind == cut::StepKind::end)
        {
            end_stream();
            output.ensure();
            return;
        }

        const auto fill = ring_fill();

        if (fill >= ring_high_watermark_frames)
        {
            output.ensure();
            pace_producer(fill);
            continue;
        }

        if (!carry_out(step, producer).is_ok())
        {
            return;
        }
    }
}

// The player's side of a play once its file is produced: commands and the
// output's checks every 2 ms, until a fault or STOP ends the play, or the
// output has drained the file. A USB play's drain must keep confirming its
// queued file frames (check_drain(), error 26).
void await_output_end() noexcept
{
    QueuedFramesWatch drain{};

    while (!faulted && !stop_requested && (player_state == PlayerState::playing || (player_state == PlayerState::draining && pending_output_drain())))
    {
        service_player_command();
        check_output();
        check_drain(drain);
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2));
    }
}

// Tear down borrowed engine resources and FatFs handles before making the
// player reusable. Preserve the copied watch identity and engine counters.
void cleanup_player_run(bool opened) noexcept
{
    stop_output();

    if (!detach_secondary_source().is_ok())
    {
        latch_fault(PlayerError::sd_read);
    }

    if (opened)
    {
        primary_close_pending = f_close(&playback_file) != FR_OK;

        if (primary_close_pending)
        {
            player_cleanup_pending = true;
            latch_fault(PlayerError::sd_read);
        }
    }

    if (!player_cleanup_pending && f_mount(nullptr, drive_path, 0U) != FR_OK)
    {
        player_cleanup_pending = true;
        latch_fault(PlayerError::sd_read);
    }

    taskENTER_CRITICAL();
    file_system_owned = player_cleanup_pending;
    run_busy = false;

    if (!faulted)
    {
        counters.pcm_drained = stop_requested ? 0U : 1U;
        player_state = stop_requested ? PlayerState::aborted : PlayerState::done;
    }

    taskEXIT_CRITICAL();
    publish_live();
}

void run_player() noexcept
{
    bool opened = false;
    const std::size_t presets = live_controller.capture_count();

    for (std::size_t i = 0; i < presets; ++i)
    {
        preset_events[i] = live_controller.capture_data()[i];
    }

    do
    {
        set_state(PlayerState::mounting);

        if (!WFG_MediaIsLocal() || f_mount(&file_system, drive_path, 1U) != FR_OK)
        {
            latch_fault(PlayerError::mount);
            break;
        }

        file_system_owned = true;
        service_player_command();

        if (stop_requested)
        {
            break;
        }

        set_state(PlayerState::opening);

        if (f_open(&playback_file, playback_path, FA_READ) != FR_OK)
        {
            latch_fault(PlayerError::open);
            break;
        }

        opened = true;
        WavPayload payload{};

        if (!parse_wav(payload).is_ok() || !seek_to(payload.offset).is_ok())
        {
            if (!faulted)
            {
                latch_fault(PlayerError::seek);
            }

            break;
        }

        // A looped item's engine is configured for its whole looped input.
        const auto card = card_source(payload);
        load_scenario(card.frames_left);

        if (faulted || stop_requested)
        {
            break;
        }

        const double reference = reference_override > 0 ? reference_override * (engine_active ? signal_lab::det::db_to_amplitude(scenario.source_gain_db) : 1.0)
                                 : engine_active        ? engine().scaled_reference_rms()
                                                        : measure_reference(payload);

        if (faulted || stop_requested)
        {
            break;
        }

        live_reference_valid = std::isfinite(reference) && reference > 0 && reference <= 1.0;

        if (!live_reference_valid && presets)
        {
            fail_live_controls(live_fault::no_reference);
            break;
        }

        // A refused preset stops the run here too, as a refused reset does: its
        // fault stands, and no output starts.
        if (!start_live_controls(live_reference_valid ? reference : 1.0, std::span{preset_events}.first(presets)).is_ok())
        {
            break;
        }

        publish_live();
        set_state(PlayerState::buffering);
        produce(card);
        await_output_end();
        // A USB stream that closed during the drain dropped the frames it had
        // not yet sent, which also ends the drain: that run did not complete.
        // A close after the last file frame was confirmed sent dropped none.
        // A looped item's anomalies end it only while it runs: once its
        // stream is delivered, or STOP has ended it, none is left to end.
        check_sink();
    } while (false);

    // One owner stops output and closes FatFs before advertising reusable state.
    cleanup_player_run(opened);
}

// The PLAYLIST sequencer (common/playlist.hpp; the player task runs it after
// accept_playlist()). Each item runs as LOAD, its CUT list and PLAY would run
// it, then its gap passes: on the USB route in stream frames, on the codec
// route in ticks.

// The gap after an item. On the USB route after an item that sent frames:
// the stream index where it ends (playlist::next_start_index()), on the
// stream the item played on. Always: the tick it began and its length in
// ticks, the codec route's measure and the fallback once that stream has
// closed or stalled.
struct GapWait
{
    std::optional<usb_audio::ScheduledStart> stream_end{};
    usb_audio::TickCount began{};
    usb_audio::TickSpan length{};
};

// What a gap counted in stream frames has seen of its stream: its frames
// sent when they last moved, and the tick then. A stream that sent nothing
// for more than usb_stall_timeout (a host that stopped polling but kept the
// stream open, as a suspended bus does) no longer counts the gap: the tick
// clock does, as after a close, so the wait always ends.
class StreamProgress
{
public:
    // Whether `stream`, as just read, has stalled by `now`: its frames sent
    // unchanged for more than usb_stall_timeout. The first call sees it move.
    [[nodiscard]] bool stalled(const usb_audio::StreamCounters& stream, usb_audio::TickCount now) noexcept
    {
        if (!seen_ || stream.frames_sent.value != frames_sent_.value)
        {
            seen_ = true;
            frames_sent_ = stream.frames_sent;
            moved_ = now;
            return false;
        }

        return now.value - moved_.value > usb_stall_timeout.value;
    }

private:
    usb_audio::FrameCount frames_sent_{};
    usb_audio::TickCount moved_{};
    bool seen_{};
};

// How an item starts: the stream index its gap ends at, when it is a USB
// item after one (the block reports it), and whether the play arms for it;
// one the tick clock shows long past is reported, not armed.
struct StartPlan
{
    std::optional<usb_audio::ScheduledStart> target{};
    bool armed_for_target{};
};

// One item's run: its result block, and the capture stream it played on
// (StreamCounters::opens once its PLAY was accepted).
struct ItemRun
{
    playlist::ResultBlock block{};
    std::uint32_t stream_opens{};
};

[[nodiscard]] usb_audio::StreamCounters usb_stream_counters() noexcept
{
    const TaskCriticalSection lock;
    return usb_audio::stream_counters();
}

[[nodiscard]] bool same_open_stream(const usb_audio::StreamCounters& stream, const usb_audio::ScheduledStart& start) noexcept
{
    return stream.state != usb_audio::StreamState::closed && stream.opens == start.stream_opens;
}

void set_progress(playlist::State state, std::uint32_t index) noexcept
{
    auto& progress = player_playlist().progress;
    const TaskCriticalSection lock;
    progress.state = state;
    progress.index = index;
}

// Sleeps `span`, at least one tick and at most playlist_poll; a command wakes
// the task sooner.
void playlist_sleep(usb_audio::TickSpan span) noexcept
{
    static_cast<void>(ulTaskNotifyTake(pdTRUE, std::clamp<TickType_t>(span.value, 1U, playlist_poll.value)));
}

// The ticks since `then`, across the tick count's wrap.
[[nodiscard]] usb_audio::TickSpan ticks_since(usb_audio::TickCount then) noexcept
{
    return {.value = current_tick().value - then.value};
}

// The ticks `gap` still lasts: in stream frames, rounded up to whole packets,
// while its stream is open and moving (`progress`); on the tick clock from
// its start otherwise.
[[nodiscard]] usb_audio::TickSpan gap_left(const GapWait& gap, StreamProgress& progress) noexcept
{
    if (gap.stream_end.has_value())
    {
        const auto stream = usb_stream_counters();

        if (same_open_stream(stream, *gap.stream_end) && !progress.stalled(stream, current_tick()))
        {
            if (usb_audio::index_reached(stream.frames_sent, gap.stream_end->index))
            {
                return {};
            }

            return ticks_from_ms(playlist::whole_chunk_frames(gap.stream_end->index.value - stream.frames_sent.value) / frames_per_millisecond);
        }
    }

    const auto elapsed = ticks_since(gap.began);
    return {.value = elapsed.value >= gap.length.value ? 0U : gap.length.value - elapsed.value};
}

// Waits until `gap` has passed, serving commands: playlist_stopped on STOP.
[[nodiscard]] m110::Status wait_gap(const GapWait& gap) noexcept
{
    StreamProgress progress{};

    for (;;)
    {
        service_player_command();

        if (player_playlist().stop_requested)
        {
            return playlist_stopped;
        }

        const auto left = gap_left(gap, progress);

        if (left.value == 0U)
        {
            return m110::Status::success();
        }

        playlist_sleep(left);
    }
}

// Before item `index`: room for its result block. A full ring pauses the
// playlist until PLAYLIST ACK frees a block: no block is ever dropped.
// playlist_stopped on STOP.
[[nodiscard]] m110::Status wait_for_result_room(std::uint32_t index) noexcept
{
    auto& list = player_playlist();

    while (list.results.full())
    {
        set_progress(playlist::State::paused_ring_full, index);
        service_player_command();

        if (list.stop_requested)
        {
            return playlist_stopped;
        }

        if (list.results.full())
        {
            playlist_sleep(playlist_poll);
        }
    }

    return m110::Status::success();
}

// How item `index` starts after `gap` (none before the first item). A USB
// item after a USB item that sent frames, on the stream still open: at once,
// for the stream index the gap ends at, so it is prepared during its gap
// (StartPlan). Otherwise once the gap has passed, as soon as it is ready.
// playlist_stopped on STOP meanwhile.
[[nodiscard]] m110::Result<StartPlan> plan_start(const std::optional<GapWait>& gap, playlist::Route route, std::uint32_t index) noexcept
{
    if (!gap.has_value())
    {
        return StartPlan{};
    }

    const auto& end = gap->stream_end;

    if (route == playlist::Route::usb && end.has_value() && same_open_stream(usb_stream_counters(), *end))
    {
        return StartPlan{.target = end, .armed_for_target = ticks_since(gap->began).value <= gap->length.value + schedule_slack.value};
    }

    set_progress(playlist::State::gap, index);
    const auto waited = wait_gap(*gap);

    if (!waited.is_ok())
    {
        return waited;
    }

    return StartPlan{};
}

// An item's result block as the item begins: which item, and how it was to
// start.
[[nodiscard]] playlist::ResultBlock new_block(std::uint32_t index, const playlist::Item& item, const StartPlan& plan) noexcept
{
    const auto scheduled_index = [](const usb_audio::ScheduledStart& start) { return start.index.value; };

    return {
        .run = player_playlist().progress.run,
        .item = index,
        .label = item.label,
        .file = item.file,
        .route = item.route,
        .scenario = item.scenario,
        .seed = item.seed,
        .scenario_crc32 = item.scenario_crc32,
        .gap = item.gap,
        .scheduled_index = plan.target.transform(scheduled_index),
        .start_tick = current_tick().value,
        .attenuation = item.attenuation,
        .duration = item.duration,
    };
}

[[nodiscard]] playlist::Outcome run_outcome(PlayerState state) noexcept
{
    if (state == PlayerState::done)
    {
        return playlist::Outcome::done;
    }

    return state == PlayerState::aborted ? playlist::Outcome::aborted : playlist::Outcome::error;
}

// The cause each player error gives an item that ended in it; any other
// error is a fault (common/playlist.hpp EndCause).
struct ErrorCause
{
    PlayerError error;
    playlist::EndCause cause;
};

constexpr auto error_causes = std::to_array<ErrorCause>({
    {.error = PlayerError::underrun, .cause = playlist::EndCause::underrun},
    {.error = PlayerError::missed_interval, .cause = playlist::EndCause::missed_interval},
    {.error = PlayerError::usb_stream_closed, .cause = playlist::EndCause::stream_closed},
    {.error = PlayerError::usb_stall, .cause = playlist::EndCause::stall},
    {.error = PlayerError::seek, .cause = playlist::EndCause::sd_read},
    {.error = PlayerError::sd_read, .cause = playlist::EndCause::sd_read},
    {.error = PlayerError::usb_assert, .cause = playlist::EndCause::usb_assert},
    {.error = PlayerError::deadline_miss, .cause = playlist::EndCause::deadline_miss},
    {.error = PlayerError::usb_no_stream, .cause = playlist::EndCause::no_stream},
});

// Why an item ended: its end, STOP, or the cause of the error it latched.
[[nodiscard]] constexpr playlist::EndCause end_cause(playlist::Outcome outcome, PlayerError error) noexcept
{
    if (outcome != playlist::Outcome::error)
    {
        return outcome == playlist::Outcome::done ? playlist::EndCause::end : playlist::EndCause::stop;
    }

    const auto found = std::ranges::find(error_causes, error, &ErrorCause::error);
    return found == error_causes.end() ? playlist::EndCause::fault : found->cause;
}

static_assert(end_cause(playlist::Outcome::done, PlayerError::none) == playlist::EndCause::end && end_cause(playlist::Outcome::aborted, PlayerError::none) == playlist::EndCause::stop &&
                  end_cause(playlist::Outcome::error, PlayerError::seek) == playlist::EndCause::sd_read &&
                  end_cause(playlist::Outcome::error, PlayerError::deadline_miss) == playlist::EndCause::deadline_miss &&
                  end_cause(playlist::Outcome::error, PlayerError::impairment) == playlist::EndCause::fault,
              "an item ends at its end, at STOP, or in the cause of its error");

// Whole passes of a card of `card_frames` frames that `output_frames` of the
// engine's output cover: a looped item's loops completed (one for a whole
// play of the card, unless its scenario slips samples).
[[nodiscard]] constexpr std::uint32_t whole_passes(std::uint32_t output_frames, std::uint32_t card_frames) noexcept
{
    return card_frames == 0U ? 0U : output_frames / card_frames;
}

// The item's play as STATUS and USB? report the current or last play now:
// the USB statistics and the cuts read together, as USB? reads them, with
// the loops it completed, why it ended and its engine deadline misses.
void record_play(playlist::ResultBlock& block) noexcept
{
    const auto status = player_snapshot();
    block.outcome = run_outcome(player_state.load());
    block.player_error = block.outcome == playlist::Outcome::error ? status.error : 0U;
    const TaskCriticalSection lock;
    const auto usb = usb_audio::play_statistics();
    const auto cuts = player_cut_statistics(usb);
    block.loops = whole_passes(cuts.output_frames, status.file_frames_total);
    block.end_cause = end_cause(block.outcome, counters.error);
    block.play = {
        .file_frames_total = status.file_frames_total,
        .usb_file_frames = cuts.usb_file_frames,
        .sent_frames = cuts.sent_frames,
        .crc32_left = usb.crc32_left,
        .crc32_right = usb.crc32_right,
        .first_frame_index = usb.first_frame_index.value,
        .last_frame_index = usb.last_frame_index.value,
        .first_frame_sof = usb.first_frame_sof.value,
        .missed_intervals = usb.missed_intervals.value,
        .first_miss_frame_index = usb.first_miss_frame_index.value,
        .stream_bits = usb.format.bits,
        .stream_channels = usb.format.channels,
        .cut_count = cuts.applied,
        .cut_drop_frames = cuts.drop_frames,
        .cut_zero_frames = cuts.zero_frames,
        .cut_first_index = first_cut_wire_index(cuts.first),
        .cut_rejected = cuts.rejected,
        .underruns = status.underruns,
        .pcm_drained = status.pcm_drained,
        .engine_state = status.engine_state,
        .engine_error = status.engine_error,
        .deadline_misses = play_watch().deadline_misses,
        .watterson = play_watch().watterson,
        .secondary_source = play_watch().secondary_source,
        .engine_digest_hi = status.engine_digest_hi,
        .engine_digest_lo = status.engine_digest_lo,
    };
}

// Item `index`: its route, then, once its output takes it, LOAD, its
// scenario (scn= or none) and seed, its CUT list and PLAY, as those commands
// set them, and its attenuation and duration, then its run (run_player()),
// which a looped item's first anomaly ends early. On the USB
// route without an open capture stream it plays nothing (usb_no_stream) and
// selects nothing: the player keeps the file, the scenario and the state the
// run before it left, so a later PLAY needs LOAD as after every other item,
// and no CUT list is left pending for it.
[[nodiscard]] ItemRun play_item(std::uint32_t index, const playlist::Item& item, const StartPlan& plan) noexcept
{
    auto& list = player_playlist();
    ItemRun run{.block = new_block(index, item, plan)};
    output_route.set_between_runs(to_output_route(item.route));

    if (const auto accepted = accept_output(); !accepted.is_ok())
    {
        player_cuts().pending.clear();
        run.block.outcome = playlist::Outcome::error;
        run.block.player_error = std::to_underlying(PlayerError::usb_no_stream);
        run.block.end_cause = end_cause(run.block.outcome, PlayerError::usb_no_stream);
        run.block.end_tick = current_tick().value;
        return run;
    }

    reset_selection(item.file.c_str());
    select_item_scenario(item.scenario);
    list.scenario_seed = item.seed;
    list.attenuation = item.attenuation;
    list.duration = item.duration;
    player_cuts().pending = item.cuts;
    run.stream_opens = usb_stream_counters().opens;
    list.start = plan.armed_for_target ? plan.target : std::nullopt;
    begin_run(no_run_id);
    play_requested = false; // this task runs it now
    run_player();
    list.start.reset();
    list.scenario_seed.reset();
    list.attenuation = attenuation::none;
    list.duration.reset();
    run.block.end_tick = current_tick().value;
    record_play(run.block);
    return run;
}

// Keeps the item's block and counts the item. The ring has room
// (wait_for_result_room()); were it full, the playlist would end in error
// rather than drop the block.
[[nodiscard]] m110::Status record_result(const playlist::ResultBlock& block) noexcept
{
    auto& list = player_playlist();
    const TaskCriticalSection lock;
    const auto kept = list.results.push(block);

    if (kept.is_ok())
    {
        ++list.progress.completed;
        list.progress.failed += block.outcome == playlist::Outcome::error ? 1U : 0U;
    }

    return kept;
}

// The gap after an item, from now.
[[nodiscard]] GapWait begin_gap(const playlist::Item& item, const ItemRun& run) noexcept
{
    GapWait gap{.began = current_tick(), .length = ticks_from_ms(playlist::gap_milliseconds(item.gap))};
    const auto& play = run.block.play;

    if (item.route == playlist::Route::usb && play.sent_frames != 0U)
    {
        const usb_audio::FrameCount end{.value = playlist::next_start_index(play.first_frame_index, play.sent_frames, item.gap)};
        gap.stream_end = usb_audio::ScheduledStart{.index = end, .stream_opens = run.stream_opens};
    }

    return gap;
}

// Item `index`, from the wait for room for its block to the start of its
// gap: the state the playlist ends in, or none when it goes on.
[[nodiscard]] std::optional<playlist::State> run_item(std::uint32_t index, std::optional<GapWait>& gap) noexcept
{
    auto& list = player_playlist();
    const auto& item = list.table.items()[index];

    if (!wait_for_result_room(index).is_ok())
    {
        return playlist::State::aborted;
    }

    const auto plan = plan_start(gap, item.route, index);

    if (!plan.has_value())
    {
        return playlist::State::aborted;
    }

    set_progress(playlist::State::running, index);
    const auto run = play_item(index, item, plan.value());

    if (!record_result(run.block).is_ok())
    {
        return playlist::State::error;
    }

    if (player_cleanup_pending)
    {
        return playlist::State::error;
    }

    const auto end = playlist::end_after(run.block.outcome, list.progress.policy, list.stop_requested);

    if (end.has_value())
    {
        return end;
    }

    gap = begin_gap(item, run);
    set_progress(playlist::State::gap, index + 1U);
    return std::nullopt;
}

// The last item's gap, so the stream (or the codec) stays silent for it
// before the playlist is done; aborted on STOP meanwhile.
[[nodiscard]] playlist::State finish_last_gap(const std::optional<GapWait>& gap) noexcept
{
    if (gap.has_value() && !wait_gap(*gap).is_ok())
    {
        return playlist::State::aborted;
    }

    return playlist::State::done;
}

// The playlist ends in `state`: the player is free again, and the progress
// and the result blocks stay for PLAYLIST?.
void end_playlist(playlist::State state) noexcept
{
    auto& list = player_playlist();
    {
        const TaskCriticalSection lock;
        list.progress.state = state;

        if (state == playlist::State::done)
        {
            list.progress.index = list.progress.items;
        }

        list.owns_player = false;
        list.stop_requested = false;
    }

    publish_live();
}

// The playlist accept_playlist() accepted: each item in order, then the last
// gap. STOP, or an item error under the stop policy, ends it early.
void run_playlist() noexcept
{
    auto& list = player_playlist();
    list.run_requested = false;
    std::optional<GapWait> gap{};
    std::optional<playlist::State> ended{};

    for (std::uint32_t index = 0U; index < list.table.size() && !ended.has_value(); ++index)
    {
        ended = run_item(index, gap);
    }

    if (ended.has_value())
    {
        end_playlist(*ended);
        return;
    }

    end_playlist(finish_last_gap(gap));
}

void player_task_entry(void*) noexcept
{
    reset_selection("PLAY.WAV");
    publish_live();

    for (;;)
    {
        service_player_command();
        service_preparation();

        if (play_requested)
        {
            play_requested = false;
            run_player();
            continue;
        }

        if (player_playlist().run_requested)
        {
            run_playlist();
            continue;
        }

        const bool work_ready = tx_artifact_busy() || (preparation_busy && preparation_snapshot.phase != prepare::Phase::cleanup_failed);
        (void)ulTaskNotifyTake(pdTRUE, work_ready ? pdMS_TO_TICKS(1) : portMAX_DELAY);
    }
}

} // namespace

m110::Status start_player() noexcept
{
    if (player_task_handle != nullptr)
    {
        return {m110::StatusCode::invalid_argument, "player already started"};
    }

    if (player_engine == nullptr)
    {
        player_engine = std::construct_at(reinterpret_cast<signal_lab::Engine*>(player_engine_storage));
    }

    if (secondary_source_provider == nullptr)
    {
        secondary_source_provider = std::construct_at(reinterpret_cast<Rt1170SecondaryWav*>(player_secondary_source_storage));
    }

    if (preparation_job == nullptr)
    {
        preparation_job = std::construct_at(reinterpret_cast<PrepareJob*>(player_prepare_storage));
    }

    // Startup leaves OCRAM's .ocram section unwritten (NOLOAD): its state is
    // written here, before the player task that owns it exists.
    player_cuts() = PlayerCuts{};
    play_watch() = PlayWatch{};
    command_mailbox() = live_protocol::Request{};
    reply_mailbox() = ControlReply{};
    reset_playlist();
    player_task_handle = xTaskCreateStatic(&player_task_entry, "wfg_player", player_task_stack_words, nullptr, player_task_priority, player_task_stack.data(), &player_task_control);

    if (player_task_handle == nullptr)
    {
        return {m110::StatusCode::unavailable, "player task creation failed"};
    }

    return m110::Status::success();
}

m110::Status request_player_play() noexcept
{
    live_protocol::Request command{};
    command.kind = live_protocol::Kind::play;
    return submit_player_command(command).is_ok() ? m110::Status::success() : m110::Status{m110::StatusCode::busy, "player command busy"};
}

m110::Status submit_player_command(const live_protocol::Request& command) noexcept
{
    taskENTER_CRITICAL();

    if (!player_task_handle || command_pending || reply_pending)
    {
        taskEXIT_CRITICAL();
        return refusal::busy;
    }

    command_mailbox() = command;
    command_pending = true;
    taskEXIT_CRITICAL();
    xTaskNotifyGive(player_task_handle);
    return m110::Status::success();
}

void notify_player_task() noexcept
{
    if (player_task_handle)
    {
        xTaskNotifyGive(player_task_handle);
    }
}

bool take_player_reply(ControlReply& reply) noexcept
{
    bool command_waiting{};
    taskENTER_CRITICAL();

    if (!reply_pending)
    {
        taskEXIT_CRITICAL();
        return false;
    }

    reply = reply_mailbox();
    reply_pending = false;
    command_waiting = command_pending;
    taskEXIT_CRITICAL();

    if (command_waiting)
    {
        notify_player_task();
    }

    return true;
}

OutputRoute player_output_route() noexcept
{
    return output_route.current();
}

PlayCutStatistics player_cut_statistics(const usb_audio::PlayStatistics& usb_play) noexcept
{
    const TaskCriticalSection lock;

    if (!play_accepted())
    {
        return {};
    }

    const auto& cuts = player_cuts();
    // The play's frames sent: those the USB stream confirmed, or those the
    // codec took from the output FIFO. Either way the FIFO's, after the cuts.
    const bool usb = cuts.route == OutputRoute::usb;
    const cut::SentFrames sent{.value = usb ? usb_play.file_frames.value : counters.file_frames_submitted};
    const auto tally = cuts.log.tally(sent);
    std::optional<FirstCut> first{};

    if (tally.first.has_value())
    {
        // A codec play has no stream index: its frames count from its first.
        first = FirstCut{.position = *tally.first, .stream_base = usb ? std::optional{usb_play.first_frame_index} : std::nullopt};
    }

    return {
        .applied = tally.applied,
        .drop_frames = tally.drop_frames.value,
        .zero_frames = tally.zero_frames.value,
        .first = first,
        .rejected = tally.rejected,
        .sent_frames = sent.value,
        .usb_file_frames = usb ? tally.output_frames.value : usb_play.file_frames.value,
        .output_frames = tally.output_frames.value,
    };
}

std::uint32_t player_deadline_misses() noexcept
{
    const TaskCriticalSection lock;
    return play_watch().deadline_misses;
}

playlist::WattersonIdentity player_watterson_identity() noexcept
{
    const TaskCriticalSection lock;
    return play_watch().watterson;
}

SecondarySourceIdentity player_secondary_source_identity() noexcept
{
    const TaskCriticalSection lock;
    return play_watch().secondary_source;
}

m110::Status player_engine_reason() noexcept
{
    const TaskCriticalSection lock;
    return play_watch().engine_reason;
}

m110::Status player_live_reason() noexcept
{
    const TaskCriticalSection lock;
    return play_watch().live_reason;
}

usb_audio::FrameCount player_consume_from_isr(usb_audio::ChunkFrames frames) noexcept
{
    UsbChunk chunk{frames};
    return usb_audio::FrameCount{.value = consume_output<IsrCriticalSection>(chunk)};
}

playlist::Snapshot player_playlist_snapshot() noexcept
{
    const auto& list = player_playlist();
    playlist::Snapshot snapshot{};
    const TaskCriticalSection lock;
    snapshot.progress = list.progress;
    snapshot.pending = list.results.size();
    snapshot.oldest_seq = list.results.oldest_seq();
    snapshot.next_seq = list.results.next_seq();
    snapshot.tick = current_tick().value;
    snapshot.block_count = list.results.copy_oldest(snapshot.blocks);
    return snapshot;
}

LiveSnapshot player_live_snapshot() noexcept
{
    taskENTER_CRITICAL();
    const auto result = live_snapshot;
    taskEXIT_CRITICAL();
    return result;
}

prepare::Snapshot player_prepare_snapshot() noexcept
{
    const TaskCriticalSection lock;
    return preparation_snapshot;
}

PreparedPlaybackDiagnostics player_prepared_playback_snapshot() noexcept
{
    const TaskCriticalSection lock;
    return prepared_playback_diagnostics;
}

bool player_allows_media_host() noexcept
{
    const bool codec_running = m110::imxrt1170::wm8960_codec().is_running();
    taskENTER_CRITICAL();
    const auto state = player_state.load();
    // An armed USB play is a run in progress: the card stays with the board.
    const bool usb_armed = usb_audio::play_statistics().armed;
    // A playlist keeps the card with the board in its gaps and pauses too.
    const bool allowed = !file_system_owned && !run_busy && !play_requested && !command_pending && !tx_artifact_busy() && !codec_running && !usb_armed && !playlist_owns_player() &&
                         (state == PlayerState::stopped || state == PlayerState::waiting_for_command || state == PlayerState::done || state == PlayerState::aborted || state == PlayerState::fault);
    taskEXIT_CRITICAL();
    return allowed;
}

PlayerSnapshot player_snapshot() noexcept
{
    taskENTER_CRITICAL();
    const PlayerSnapshot snapshot{
        .state = static_cast<std::uint32_t>(player_state.load()),
        .error = static_cast<std::uint32_t>(counters.error),
        .data_bytes = counters.data_bytes,
        .file_frames_total = counters.file_frames_total,
        .file_frames_enqueued = counters.file_frames_enqueued,
        .ring_fill_frames = ring_write_total - ring_read_total,
        .ring_min_frames = counters.ring_min_frames,
        .ring_min_pre_eof_frames = counters.ring_min_pre_eof_frames,
        .ring_max_frames = counters.ring_max_frames,
        .frames_requested = counters.frames_requested,
        .file_frames_submitted = counters.file_frames_submitted,
        .pcm_drained = counters.pcm_drained,
        .eof_count = counters.eof_count,
        .eof_silence_frames = counters.eof_silence_frames,
        .sd_read_calls = counters.sd_read_calls,
        .sd_bytes_read = counters.sd_bytes_read,
        .sd_short_reads = counters.sd_short_reads,
        .sd_read_errors = counters.sd_read_errors,
        .underruns = counters.underruns,
        .first_underrun_frame = counters.first_underrun_frame,
        .max_sd_read_cycles = counters.max_sd_read_cycles,
        .engine_state = engine_counters.state,
        .engine_error = engine_counters.error,
        .engine_stages = engine_counters.stages,
        .engine_frames_in = engine_counters.frames_in,
        .engine_frames_out = engine_counters.frames_out,
        .engine_clipped = engine_counters.clipped,
        .engine_digest_hi = engine_counters.digest_hi,
        .engine_digest_lo = engine_counters.digest_lo,
        .engine_source_digest_hi = engine_counters.source_digest_hi,
        .engine_source_digest_lo = engine_counters.source_digest_lo,
        .engine_max_block_cycles = engine_counters.max_block_cycles,
        .engine_events_applied = engine_counters.events_applied,
        .engine_events_dropped = engine_counters.events_dropped,
        .engine_arena_bytes = engine_counters.arena_bytes,
        .ring_capacity_frames = ring_capacity_frames,
        .ring_high_watermark_frames = ring_high_watermark_frames,
        .ring_wake_watermark_frames = ring_wake_watermark_frames,
        .ring_critical_frames = ring_critical_frames,
        .ring_avg_frames = counters.ring_fill_samples != 0U ? static_cast<std::uint32_t>(counters.ring_fill_sum / counters.ring_fill_samples) : 0U,
        .ring_critical_events = counters.ring_critical_events,
        .producer_rate_sps = counters.producer_active_cycles != 0U
                                 ? static_cast<std::uint32_t>((static_cast<std::uint64_t>(counters.producer_frames) * m110::platform_cycle_counter_frequency_hz()) / counters.producer_active_cycles)
                                 : 0U,
        .producer_worst_block_cycles = counters.producer_worst_block_cycles,
        .producer_sleeps = counters.producer_sleeps,
    };
    taskEXIT_CRITICAL();
    return snapshot;
}

} // namespace waveform_generator
