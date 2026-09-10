#include "player.hpp"
#include "tx_artifact.hpp"

#include "common/wav.hpp"
#include "common/pcm24.hpp"
#include "common/live_command.hpp"
#include "platform/board.hpp"
#include "platform/waveform_msc.h"
#include "platform/wm8960_codec.hpp"
#include "signal_lab/mixer.hpp"
#include "signal_lab/scenario.hpp"

extern "C"
{
#include "FreeRTOS.h"
#include "ff.h"
#include "fsl_common.h"
#include "task.h"
}

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cmath>

namespace waveform_generator
{
namespace
{

constexpr char drive_path[] = "2:/";
char playback_path[24] = "2:/WG/PLAY.WAV";
char scenario_path[24] = "2:/WG/PLAY.SCN";
constexpr std::uint32_t scenario_capacity = 8192U;
// The impairment engine runs in this task; its deterministic math and the FatFs
// scenario read need more headroom than the original 1536-word P1.2 stack.
constexpr std::uint32_t player_task_stack_words = 4096U;
constexpr UBaseType_t player_task_priority = 4U;
constexpr std::uint32_t codec_frames_per_block = 128U;
// Post-impairment output FIFO in OCRAM2: the only large buffer in the chain and the
// hard real-time boundary. The producer (SD read -> impairment engine) runs ahead of
// real time until the high watermark, sleeps until the wake watermark, and is never
// deadline-bound per block; only the audio hook draining this ring has a deadline.
constexpr std::uint32_t ring_capacity_frames = 32768U;                      // 683 ms
constexpr std::uint32_t ring_mask = ring_capacity_frames - 1U;
constexpr std::uint32_t ring_high_watermark_frames = 24000U;                // 500 ms: stop producing
constexpr std::uint32_t ring_wake_watermark_frames = 14400U;                // 300 ms: resume producing
constexpr std::uint32_t ring_critical_frames = 4800U;                       // 100 ms: count as a near-miss
// One SD chunk = one engine block of packed little-endian PCM24 frames.
constexpr std::uint32_t read_buffer_bytes = 3U * signal_lab::engine_block_frames;
constexpr std::uint32_t eof_drain_frames = 3U * codec_frames_per_block;

static_assert((ring_capacity_frames & ring_mask) == 0U);
static_assert((read_buffer_bytes % waveform_generator::audio::pcm24_bytes_per_sample) == 0U);
static_assert(ring_high_watermark_frames + 2U * signal_lab::engine_capacity_frames <= ring_capacity_frames,
              "a full engine block must always fit above the high watermark");
static_assert(ring_wake_watermark_frames < ring_high_watermark_frames && ring_critical_frames < ring_wake_watermark_frames);

struct RuntimeCounters
{
    volatile PlayerState state{PlayerState::stopped};
    volatile PlayerError error{PlayerError::none};
    volatile std::uint32_t data_bytes{};
    volatile std::uint32_t file_frames_total{};
    volatile std::uint32_t file_frames_enqueued{};
    volatile std::uint32_t ring_min_frames{ring_capacity_frames};
    volatile std::uint32_t ring_min_pre_eof_frames{ring_capacity_frames};
    volatile std::uint32_t ring_max_frames{};
    volatile std::uint32_t frames_requested{};
    volatile std::uint32_t file_frames_submitted{};
    volatile std::uint32_t pcm_drained{};
    volatile std::uint32_t eof_count{};
    volatile std::uint32_t eof_silence_frames{};
    volatile std::uint32_t sd_read_calls{};
    volatile std::uint32_t sd_bytes_read{};
    volatile std::uint32_t sd_short_reads{};
    volatile std::uint32_t sd_read_errors{};
    volatile std::uint32_t underruns{};
    volatile std::uint32_t first_underrun_frame{};
    volatile std::uint32_t max_sd_read_cycles{};
    // Output-FIFO depth statistics sampled by the audio hook, and producer pacing.
    volatile std::uint64_t ring_fill_sum{};
    volatile std::uint32_t ring_fill_samples{};
    volatile std::uint32_t ring_critical_events{};
    volatile bool ring_below_critical{};
    volatile std::uint64_t producer_active_cycles{};
    volatile std::uint32_t producer_frames{};
    volatile std::uint32_t producer_worst_block_cycles{};
    volatile std::uint32_t producer_sleeps{};
};

struct EngineCounters
{
    volatile std::uint32_t state{};
    volatile std::uint32_t error{};
    volatile std::uint32_t stages{};
    volatile std::uint32_t frames_in{};
    volatile std::uint32_t frames_out{};
    volatile std::uint32_t clipped{};
    volatile std::uint32_t digest_hi{};
    volatile std::uint32_t digest_lo{};
    volatile std::uint32_t source_digest_hi{};
    volatile std::uint32_t source_digest_lo{};
    volatile std::uint32_t max_block_cycles{};
    volatile std::uint32_t events_applied{};
    volatile std::uint32_t events_dropped{};
    volatile std::uint32_t arena_bytes{};
};

// The output FIFO lives in OCRAM2 (.ocram, NOLOAD; cmake/MIMXRT1176xxxxx_cm7_flexspi_nor_wfg.ld).
// Only the CPU touches it (producer writes, audio hook reads), so cacheability is harmless.
__attribute__((section(".ocram.output_ring"))) alignas(32) waveform_generator::audio::Pcm24Sample sample_ring[ring_capacity_frames];
alignas(32) std::array<std::uint8_t, read_buffer_bytes> read_buffer{};
// Phase 3 impairment engine state: static, sized at compile time, never touched by the audio hook.
signal_lab::Engine engine{};
signal_lab::Scenario scenario{};
alignas(32) std::array<float, signal_lab::engine_capacity_frames> engine_input{};
alignas(32) std::array<float, signal_lab::engine_capacity_frames> engine_output{};
std::array<char, scenario_capacity> scenario_text{};
EngineCounters engine_counters{};
bool engine_active{};
StaticTask_t player_task_control{};
StackType_t player_task_stack[player_task_stack_words]{};
TaskHandle_t player_task_handle{};
FATFS file_system{};
FIL playback_file{};
RuntimeCounters counters{};
volatile std::uint32_t ring_write_total{};
volatile std::uint32_t ring_read_total{};
volatile bool eof_enqueued{};
volatile bool faulted{};
volatile bool play_requested{};
volatile bool file_system_owned{};
volatile bool run_busy{};
volatile bool stop_requested{};
signal_lab::LiveController live_controller{};
signal_lab::ControlEvent preset_events[signal_lab::live_capture_capacity]{};
LiveSnapshot live_snapshot{};
double reference_override{};
bool live_reference_valid{};
std::uint64_t live_seed{};
std::uint32_t run_number{};
live_protocol::Request command_mailbox{};
ControlReply reply_mailbox{};
volatile bool command_pending{};
volatile bool reply_pending{};

void service_player_command() noexcept;

void publish_live() noexcept
{
    taskENTER_CRITICAL();
    live_snapshot.busy = run_busy;
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
    counters.state = state;
    taskEXIT_CRITICAL();
}

void latch_fault(PlayerError error) noexcept
{
    taskENTER_CRITICAL();
    if (!faulted)
    {
        faulted = true;
        counters.error = error;
        counters.state = PlayerState::fault;
    }
    taskEXIT_CRITICAL();
}

void record_read(FRESULT result, UINT requested, UINT actual,
                 std::uint32_t elapsed_cycles) noexcept
{
    taskENTER_CRITICAL();
    counters.sd_read_calls = counters.sd_read_calls + 1U;
    counters.sd_bytes_read = counters.sd_bytes_read + actual;
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

bool read_exact(void* destination, UINT bytes) noexcept
{
    UINT bytes_read{};
    const auto begin = m110::platform_cycle_counter();
    const auto result = f_read(&playback_file, destination, bytes, &bytes_read);
    const auto elapsed = m110::platform_cycle_counter() - begin;
    record_read(result, bytes, bytes_read, elapsed);
    return result == FR_OK && bytes_read == bytes;
}

bool seek_to(std::uint32_t offset) noexcept
{
    return f_lseek(&playback_file, static_cast<FSIZE_t>(offset)) == FR_OK;
}

bool read_wav_bytes(void*, std::uint32_t offset, std::uint8_t* destination,
                    std::uint32_t bytes) noexcept
{
    if (!seek_to(offset))
    {
        return false;
    }
    return read_exact(destination, static_cast<UINT>(bytes));
}

bool parse_wav(WavPayload& payload) noexcept
{
    const WavReader reader{
        .context = nullptr,
        .file_size = static_cast<std::uint32_t>(f_size(&playback_file)),
        .read = &read_wav_bytes,
    };

    const auto result = validate_pcm24_mono_48k_wav(reader, payload);
    switch (result)
    {
    case WavValidationError::none:
        break;
    case WavValidationError::io:
        latch_fault(PlayerError::header_io);
        return false;
    case WavValidationError::invalid_riff:
        latch_fault(PlayerError::invalid_riff);
        return false;
    case WavValidationError::missing_format:
        latch_fault(PlayerError::missing_format);
        return false;
    case WavValidationError::missing_data:
        latch_fault(PlayerError::missing_data);
        return false;
    case WavValidationError::unsupported_format:
        latch_fault(PlayerError::unsupported_format);
        return false;
    }

    taskENTER_CRITICAL();
    counters.data_bytes = payload.bytes;
    counters.file_frames_total = payload.bytes / audio::pcm24_bytes_per_sample;
    taskEXIT_CRITICAL();
    return true;
}

void zero_playback(std::uint32_t* playback, std::size_t first_frame,
                   std::size_t frames) noexcept
{
    for (std::size_t frame = first_frame; frame < frames; ++frame)
    {
        playback[2U * frame] = 0U;
        playback[2U * frame + 1U] = 0U;
    }
}

void player_audio_hook(void*, const std::uint32_t*, std::uint32_t* playback,
                       std::size_t frames) noexcept
{
    if (faulted || stop_requested)
    {
        zero_playback(playback, 0U, frames);
        taskENTER_CRITICAL();
        counters.frames_requested =
            counters.frames_requested + static_cast<std::uint32_t>(frames);
        taskEXIT_CRITICAL();
        return;
    }

    if (counters.state == PlayerState::draining ||
        counters.state == PlayerState::done)
    {
        zero_playback(playback, 0U, frames);
        taskENTER_CRITICAL();
        counters.frames_requested =
            counters.frames_requested + static_cast<std::uint32_t>(frames);
        counters.eof_silence_frames =
            counters.eof_silence_frames + static_cast<std::uint32_t>(frames);
        taskEXIT_CRITICAL();
        return;
    }

    const auto read_start = ring_read_total;
    const auto write_snapshot = ring_write_total;
    __DMB();
    const auto available = write_snapshot - read_start;
    auto copied = static_cast<std::uint32_t>(frames);
    if (available < copied)
    {
        copied = available;
    }

    if (copied < frames && !eof_enqueued)
    {
        zero_playback(playback, 0U, frames);
        taskENTER_CRITICAL();
        counters.underruns = counters.underruns + 1U;
        if (counters.underruns == 1U)
        {
            counters.first_underrun_frame = counters.frames_requested;
        }
        counters.frames_requested =
            counters.frames_requested + static_cast<std::uint32_t>(frames);
        taskEXIT_CRITICAL();
        latch_fault(PlayerError::underrun);
        return;
    }

    for (std::uint32_t frame = 0U; frame < copied; ++frame)
    {
        const auto sample = sample_ring[(read_start + frame) & ring_mask];
        // WM8960 uses 24 valid MSBs in each configured 32-bit I2S slot.
        const auto word = static_cast<std::uint32_t>(sample) << 8U;
        playback[2U * frame] = word;
        playback[2U * frame + 1U] = word;
    }
    zero_playback(playback, copied, frames);

    __DMB();
    taskENTER_CRITICAL();
    ring_read_total = read_start + copied;
    counters.frames_requested =
        counters.frames_requested + static_cast<std::uint32_t>(frames);
    counters.file_frames_submitted =
        counters.file_frames_submitted + copied;
    const auto fill_after = ring_write_total - ring_read_total;
    if (fill_after < counters.ring_min_frames)
    {
        counters.ring_min_frames = fill_after;
    }
    if (!eof_enqueued && fill_after < counters.ring_min_pre_eof_frames)
    {
        counters.ring_min_pre_eof_frames = fill_after;
    }
    // Depth statistics: one sample per audio block gives the average depth and
    // counts each excursion below the critical level (before EOF) as one event.
    counters.ring_fill_sum = counters.ring_fill_sum + fill_after;
    counters.ring_fill_samples = counters.ring_fill_samples + 1U;
    if (!eof_enqueued)
    {
        if (fill_after < ring_critical_frames)
        {
            if (!counters.ring_below_critical)
            {
                counters.ring_below_critical = true;
                counters.ring_critical_events = counters.ring_critical_events + 1U;
            }
        }
        else
        {
            counters.ring_below_critical = false;
        }
    }
    if (copied < frames)
    {
        counters.eof_silence_frames = counters.eof_silence_frames +
                                      static_cast<std::uint32_t>(frames) - copied;
        counters.eof_count = counters.eof_count + 1U;
        counters.state = PlayerState::draining;
    }
    taskEXIT_CRITICAL();
}

bool start_codec() noexcept
{
    auto& codec = m110::imxrt1170::wm8960_codec();
    m110::imxrt1170::CodecConfig configuration;
    configuration.frames_per_block = codec_frames_per_block;
    const auto configured = codec.is_configured() ? m110::Status::success() : codec.configure(configuration, &player_audio_hook, nullptr);
    if (!configured.is_ok())
    {
        latch_fault(PlayerError::codec_config);
        return false;
    }

    const auto level = codec.set_transmit_level_percent(70U);
    if (!level)
    {
        latch_fault(PlayerError::codec_level);
        return false;
    }

    taskENTER_CRITICAL();
    counters.ring_min_frames = ring_write_total - ring_read_total;
    counters.ring_min_pre_eof_frames = ring_write_total - ring_read_total;
    taskEXIT_CRITICAL();
    set_state(PlayerState::playing);
    const auto started = codec.start();
    if (!started.is_ok())
    {
        latch_fault(PlayerError::codec_start);
        return false;
    }
    return true;
}

std::uint32_t push_samples(const float* source, std::uint32_t source_frames) noexcept
{
    const auto write_start = ring_write_total;
    const auto read_snapshot = ring_read_total;
    const auto used = write_start - read_snapshot;
    if (used >= ring_capacity_frames)
    {
        return 0U;
    }

    auto frames = source_frames;
    const auto free_frames = ring_capacity_frames - used;
    if (frames > free_frames)
    {
        frames = free_frames;
    }
    const auto ring_offset = write_start & ring_mask;
    const auto contiguous_frames = ring_capacity_frames - ring_offset;
    if (frames > contiguous_frames)
    {
        frames = contiguous_frames;
    }
    if (frames == 0U)
    {
        return 0U;
    }

    for (std::uint32_t frame = 0U; frame < frames; ++frame)
    {
        sample_ring[ring_offset + frame] = signal_lab::det::quantize_pcm24(source[frame]);
    }
    __DMB();
    taskENTER_CRITICAL();
    ring_write_total = write_start + frames;
    counters.file_frames_enqueued = counters.file_frames_enqueued + frames;
    const auto fill_after = ring_write_total - ring_read_total;
    if (fill_after > counters.ring_max_frames)
    {
        counters.ring_max_frames = fill_after;
    }
    taskEXIT_CRITICAL();
    return frames;
}

bool read_payload_chunk(std::uint32_t bytes_remaining,
                        std::uint32_t& bytes_staged) noexcept
{
    auto requested = bytes_remaining;
    if (requested > read_buffer.size())
    {
        requested = static_cast<std::uint32_t>(read_buffer.size());
    }

    UINT actual{};
    const auto begin = m110::platform_cycle_counter();
    const auto result = f_read(&playback_file, read_buffer.data(),
                               static_cast<UINT>(requested), &actual);
    const auto elapsed = m110::platform_cycle_counter() - begin;
    record_read(result, static_cast<UINT>(requested), actual, elapsed);
    if (result != FR_OK || actual != requested)
    {
        latch_fault(PlayerError::sd_read);
        return false;
    }
    bytes_staged = actual;
    return true;
}

void set_engine_error(EngineError error) noexcept
{
    taskENTER_CRITICAL();
    engine_counters.state = 2U;
    engine_counters.error = static_cast<std::uint32_t>(error);
    taskEXIT_CRITICAL();
}

void publish_engine_counters(std::uint32_t block_cycles) noexcept
{
    const signal_lab::EngineStats& stats = engine.stats();
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

// Load 2:/WG/PLAY.SCN if present and configure the engine for this WAV. A missing
// scenario means clean pass-through; a rejected one is reported through STATUS
// (engine_state=2, engine_error) and playback continues unimpaired so the card
// never becomes unplayable because of a bad document.
FIL scenario_file{}; // static: a FIL carries a sector buffer and must not live on the task stack

void load_scenario(std::uint32_t file_frames) noexcept
{
    engine_active = false;
    const FRESULT opened = f_open(&scenario_file, scenario_path, FA_READ);
    if (opened == FR_NO_FILE || opened == FR_NO_PATH)
    {
        return;
    }
    if (opened != FR_OK)
    {
        set_engine_error(EngineError::scenario_open);
        return;
    }
    const FSIZE_t size = f_size(&scenario_file);
    if (size >= scenario_text.size())
    {
        (void)f_close(&scenario_file);
        set_engine_error(EngineError::scenario_too_large);
        return;
    }
    UINT bytes_read{};
    const FRESULT read = f_read(&scenario_file, scenario_text.data(), static_cast<UINT>(size), &bytes_read);
    (void)f_close(&scenario_file);
    if (read != FR_OK || bytes_read != static_cast<UINT>(size))
    {
        set_engine_error(EngineError::scenario_read);
        return;
    }
    scenario_text[bytes_read] = '\0';
    const char* error = nullptr;
    if (!signal_lab::parse_scenario(scenario_text.data(), bytes_read, scenario, &error))
    {
        set_engine_error(EngineError::scenario_parse);
        return;
    }
    if (!scenario.has_reference_rms)
    {
        set_engine_error(EngineError::missing_reference_rms);
        return;
    }
    if (!engine.configure(scenario, scenario.reference_rms, file_frames, &error))
    {
        set_engine_error(EngineError::configure_failed);
        return;
    }
    taskENTER_CRITICAL();
    engine_counters.state = 1U;
    engine_counters.error = 0U;
    engine_counters.stages = engine.stats().stage_count;
    engine_counters.arena_bytes = static_cast<std::uint32_t>(engine.stats().arena_bytes_used);
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


bool stopped_state() noexcept
{
    return !run_busy && !file_system_owned && !play_requested;
}

void reset_selection(const char* name) noexcept
{
    std::memcpy(playback_path, "2:/WG/", 6);
    std::strcpy(playback_path + 6, name);
    std::strcpy(scenario_path, playback_path);
    std::strcpy(std::strrchr(scenario_path, '.'), ".SCN");
    reference_override = 0.0;
    live_reference_valid = false;
    (void)live_controller.reset(live_seed, 1.0);
    taskENTER_CRITICAL();
    std::strcpy(live_snapshot.selected_file, name);
    counters.state = PlayerState::waiting_for_command;
    taskEXIT_CRITICAL();
}

void service_player_command() noexcept
{
    service_tx_artifact();
    taskENTER_CRITICAL();
    if (!command_pending) { taskEXIT_CRITICAL(); return; }
    const auto command = command_mailbox;
    command_pending = false;
    taskEXIT_CRITICAL();
    ControlReply reply{};
    reply.seq = command.seq;
    using live_protocol::Kind;
    if (live_protocol::is_control(command.kind))
    {
        if ((!run_busy && counters.state != PlayerState::waiting_for_command) ||
            (run_busy && counters.state != PlayerState::buffering && counters.state != PlayerState::playing) ||
            (run_busy && (eof_enqueued || stop_requested || !live_reference_valid)))
            reply.error = "BAD_STATE";
        else
        {
            const auto result = live_protocol::apply_control(command, live_controller, reply.apply_frame, reply.events);
            reply.ok = result == signal_lab::ControlResult::accepted;
            reply.error = signal_lab::control_result_name(result);
        }
    }
    else if (command.kind == Kind::stop)
    {
        if (run_busy) stop_requested = true;
        else abort_tx_artifact();
        reply.ok = true;
    }
    else if (!stopped_state()) reply.error = "BUSY";
    else if (command.kind == Kind::load)
    {
        reset_selection(command.name);
        reply.ok = true;
    }
    else if (command.kind == Kind::seed || command.kind == Kind::reference)
    {
        // Do not silently erase an acknowledged control schedule.
        if (live_controller.capture_count() != 0) reply.error = "LOAD_REQUIRED";
        else
        {
            if (command.kind == Kind::seed) live_seed = command.integer;
            else reference_override = command.value;
            (void)live_controller.reset(live_seed, reference_override > 0 ? reference_override : 1.0);
            reply.ok = true;
        }
    }
    else if (command.kind == Kind::play)
    {
        if (counters.state != PlayerState::waiting_for_command || !WFG_MediaIsLocal() || tx_artifact_busy()) reply.error = "BAD_STATE";
        else if (command.run_id[0] && std::strcmp(command.run_id, live_snapshot.run_id) == 0) reply.error = "RUN_ID_REUSED";
        else
        {
            taskENTER_CRITICAL();
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
            if (command.run_id[0]) std::strcpy(live_snapshot.run_id, command.run_id);
            else
            {
                std::memset(live_snapshot.run_id, '0', 32);
                constexpr char hex[] = "0123456789abcdef";
                for (unsigned i = 0; i < 8; ++i) live_snapshot.run_id[31 - i] = hex[(run_number >> (4 * i)) & 15];
                live_snapshot.run_id[32] = 0;
            }
            counters.state = PlayerState::mounting;
            taskEXIT_CRITICAL();
            reply.ok = true;
        }
    }
    else reply.error = "BAD_REQUEST";
    publish_live();
    taskENTER_CRITICAL();
    reply_mailbox = reply;
    reply_pending = true;
    taskEXIT_CRITICAL();
}

// Deterministic fallback level: RMS of the first min(N,48000) clean PCM frames.
// Scenario reference_rms and explicit REFERENCE override this measurement.
double measure_reference(const WavPayload& payload) noexcept
{
    constexpr std::uint32_t one_second_bytes =
        48000U * waveform_generator::audio::pcm24_bytes_per_sample;
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
                waveform_generator::audio::read_pcm24_le(
                    read_buffer.data() + i * waveform_generator::audio::pcm24_bytes_per_sample));
            energy += value * value;
        }
        remaining -= actual;
    }
    if (!seek_to(payload.offset)) latch_fault(PlayerError::seek);
    return frames ? std::sqrt(energy / frames) : 0;
}

void run_player() noexcept
{
    bool opened = false;
    bool codec_started = false;
    const std::size_t presets = live_controller.capture_count();
    for (std::size_t i = 0; i < presets; ++i) preset_events[i] = live_controller.capture_data()[i];
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
        if (stop_requested) break;
        set_state(PlayerState::opening);
        if (f_open(&playback_file, playback_path, FA_READ) != FR_OK)
        {
            latch_fault(PlayerError::open);
            break;
        }
        opened = true;
        WavPayload payload{};
        if (!parse_wav(payload) || !seek_to(payload.offset))
        {
            if (!faulted) latch_fault(PlayerError::seek);
            break;
        }
        load_scenario(payload.bytes / waveform_generator::audio::pcm24_bytes_per_sample);
        const double reference = reference_override > 0 ? reference_override * (engine_active ? signal_lab::det::db_to_amplitude(scenario.source_gain_db) : 1.0)
                               : engine_active ? engine.scaled_reference_rms() : measure_reference(payload);
        if (faulted || stop_requested) break;
        live_reference_valid = std::isfinite(reference) && reference > 0 && reference <= 1.0;
        if (!live_reference_valid && presets) { latch_fault(PlayerError::live_control); break; }
        if (!live_controller.reset(live_seed, live_reference_valid ? reference : 1.0))
        {
            latch_fault(PlayerError::live_control);
            break;
        }
        for (std::size_t i = 0; i < presets; ++i)
            if (live_controller.enqueue(preset_events[i]) != signal_lab::ControlResult::accepted) latch_fault(PlayerError::live_control);
        publish_live();
        set_state(PlayerState::buffering);
        std::uint32_t bytes_remaining = payload.bytes;
        while (!faulted && !stop_requested)
        {
            service_player_command();
            if (stop_requested) break;
            if (bytes_remaining == 0)
            {
                __DMB(); eof_enqueued = true; __DMB();
                if (!codec_started) codec_started = start_codec();
                break;
            }
            const auto fill = ring_fill();
            if (fill >= ring_high_watermark_frames)
            {
                if (!codec_started) codec_started = start_codec();
                counters.producer_sleeps = counters.producer_sleeps + 1U;
                const auto surplus = fill - ring_wake_watermark_frames;
                // A control notification interrupts pacing, without making the producer deadline-bound.
                (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(surplus / 48U > 0 ? surplus / 48U : 1U));
                continue;
            }
            const auto block_begin = m110::platform_cycle_counter();
            std::uint32_t bytes_staged{};
            if (!read_payload_chunk(bytes_remaining, bytes_staged)) break;
            bytes_remaining -= bytes_staged;
            const auto frames = static_cast<std::size_t>(
                bytes_staged / waveform_generator::audio::pcm24_bytes_per_sample);
            for (std::size_t frame = 0U; frame < frames; ++frame)
            {
                engine_input[frame] = waveform_generator::audio::pcm24_to_float(
                    waveform_generator::audio::read_pcm24_le(
                        read_buffer.data() + frame * waveform_generator::audio::pcm24_bytes_per_sample));
            }
            std::size_t produced = frames;
            if (engine_active)
            {
                const auto begin = m110::platform_cycle_counter();
                produced = engine.process(engine_input.data(), frames, engine_output.data(), engine_output.size());
                publish_engine_counters(m110::platform_cycle_counter() - begin);
                if (engine.process_error() != signal_lab::ProcessError::none)
                {
                    engine_counters.state = 3;
                    engine_counters.error = static_cast<std::uint32_t>(engine.process_error() == signal_lab::ProcessError::clipping ? EngineError::clipping_rejected : EngineError::processing_failed);
                    latch_fault(PlayerError::impairment);
                    break;
                }
            }
            else std::copy_n(engine_input.data(), frames, engine_output.data());
            if (!live_controller.process(engine_output.data(), produced))
            {
                latch_fault(PlayerError::live_control);
                break;
            }
            publish_live();
            std::uint32_t staged_offset{};
            auto staged_frames = static_cast<std::uint32_t>(produced);
            while (staged_frames && !faulted && !stop_requested)
            {
                const auto pushed = push_samples(engine_output.data() + staged_offset, staged_frames);
                if (!pushed) { vTaskDelay(1); continue; }
                staged_offset += pushed;
                staged_frames -= pushed;
            }
            const auto cycles = m110::platform_cycle_counter() - block_begin;
            taskENTER_CRITICAL();
            counters.producer_active_cycles = counters.producer_active_cycles + cycles;
            counters.producer_frames = counters.producer_frames + staged_offset;
            if (cycles > counters.producer_worst_block_cycles) counters.producer_worst_block_cycles = cycles;
            taskEXIT_CRITICAL();
        }
        while (!faulted && !stop_requested &&
               (counters.state == PlayerState::playing ||
                (counters.state == PlayerState::draining && counters.eof_silence_frames < eof_drain_frames)))
        {
            service_player_command();
            (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2));
        }
    } while (false);
    // One owner stops audio and closes FatFs before advertising reusable stopped state.
    stop_codec_after_fault();
    if (opened) (void)f_close(&playback_file);
    (void)f_mount(nullptr, drive_path, 0U);
    taskENTER_CRITICAL();
    file_system_owned = false;
    run_busy = false;
    if (!faulted)
    {
        counters.pcm_drained = stop_requested ? 0U : 1U;
        counters.state = stop_requested ? PlayerState::aborted : PlayerState::done;
    }
    taskEXIT_CRITICAL();
    publish_live();
}

void player_task_entry(void*) noexcept
{
    reset_selection("PLAY.WAV");
    publish_live();
    for (;;)
    {
        service_player_command();
        if (play_requested)
        {
            play_requested = false;
            run_player();
            continue;
        }
        (void)ulTaskNotifyTake(pdTRUE, tx_artifact_busy() ? pdMS_TO_TICKS(1) : portMAX_DELAY);
    }
}

} // namespace

m110::Status start_player_checkpoint() noexcept
{
    if (player_task_handle != nullptr)
    {
        return {m110::StatusCode::invalid_argument, "player already started"};
    }

    player_task_handle = xTaskCreateStatic(
        &player_task_entry, "wfg_player", player_task_stack_words, nullptr,
        player_task_priority, player_task_stack, &player_task_control);
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
    return submit_player_command(command) ? m110::Status::success() : m110::Status{m110::StatusCode::busy, "player command busy"};
}

bool submit_player_command(const live_protocol::Request& command) noexcept
{
    taskENTER_CRITICAL();
    if (!player_task_handle || command_pending || reply_pending) { taskEXIT_CRITICAL(); return false; }
    command_mailbox = command;
    command_pending = true;
    taskEXIT_CRITICAL();
    xTaskNotifyGive(player_task_handle);
    return true;
}

void notify_player_task() noexcept
{
    if (player_task_handle) xTaskNotifyGive(player_task_handle);
}

bool take_player_reply(ControlReply& reply) noexcept
{
    taskENTER_CRITICAL();
    if (!reply_pending) { taskEXIT_CRITICAL(); return false; }
    reply = reply_mailbox;
    reply_pending = false;
    taskEXIT_CRITICAL();
    return true;
}

LiveSnapshot player_live_snapshot() noexcept
{
    taskENTER_CRITICAL();
    const auto result = live_snapshot;
    taskEXIT_CRITICAL();
    return result;
}

bool player_allows_media_host() noexcept
{
    const bool codec_running = m110::imxrt1170::wm8960_codec().is_running();
    taskENTER_CRITICAL();
    const auto state = counters.state;
    const bool allowed =
        !file_system_owned && !run_busy && !play_requested && !command_pending && !tx_artifact_busy() &&
        !codec_running &&
        (state == PlayerState::stopped ||
         state == PlayerState::waiting_for_command ||
         state == PlayerState::done ||
         state == PlayerState::aborted ||
         state == PlayerState::fault);
    taskEXIT_CRITICAL();
    return allowed;
}

PlayerSnapshot player_snapshot() noexcept
{
    taskENTER_CRITICAL();
    const PlayerSnapshot snapshot{
        .state = static_cast<std::uint32_t>(counters.state),
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
        .ring_avg_frames = counters.ring_fill_samples != 0U
                               ? static_cast<std::uint32_t>(counters.ring_fill_sum / counters.ring_fill_samples)
                               : 0U,
        .ring_critical_events = counters.ring_critical_events,
        .producer_rate_sps = counters.producer_active_cycles != 0U
                                 ? static_cast<std::uint32_t>((static_cast<std::uint64_t>(counters.producer_frames) *
                                                               m110::platform_cycle_counter_frequency_hz()) /
                                                              counters.producer_active_cycles)
                                 : 0U,
        .producer_worst_block_cycles = counters.producer_worst_block_cycles,
        .producer_sleeps = counters.producer_sleeps,
    };
    taskEXIT_CRITICAL();
    return snapshot;
}

} // namespace waveform_generator
