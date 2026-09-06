#include "player.hpp"

#include "common/wav.hpp"
#include "platform/board.hpp"
#include "platform/waveform_msc.h"
#include "platform/wm8960_codec.hpp"

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

namespace waveform_generator
{
namespace
{

constexpr char drive_path[] = "2:/";
constexpr char playback_path[] = "2:/WG/PLAY.WAV";
constexpr std::uint32_t player_task_stack_words = 1536U;
constexpr UBaseType_t player_task_priority = 4U;
constexpr std::uint32_t codec_frames_per_block = 128U;
constexpr std::uint32_t ring_capacity_frames = 32768U;
constexpr std::uint32_t ring_mask = ring_capacity_frames - 1U;
constexpr std::uint32_t prefill_frames = ring_capacity_frames / 2U;
constexpr std::uint32_t read_buffer_bytes = 8192U;
constexpr std::uint32_t eof_drain_frames = 3U * codec_frames_per_block;

static_assert((ring_capacity_frames & ring_mask) == 0U);
static_assert((read_buffer_bytes % sizeof(std::int16_t)) == 0U);

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
};

alignas(32) std::array<std::int16_t, ring_capacity_frames> sample_ring{};
alignas(32) std::array<std::uint8_t, read_buffer_bytes> read_buffer{};
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

    const auto result = validate_pcm16_mono_48k_wav(reader, payload);
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
    counters.file_frames_total = payload.bytes / sizeof(std::int16_t);
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
    if (faulted)
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
        const auto sample = static_cast<std::int32_t>(
            sample_ring[(read_start + frame) & ring_mask]);
        const auto word = static_cast<std::uint32_t>(sample) << 16U;
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
    const auto configured = codec.configure(configuration, &player_audio_hook, nullptr);
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

std::uint32_t push_samples(const std::uint8_t* source,
                           std::uint32_t source_bytes) noexcept
{
    const auto write_start = ring_write_total;
    const auto read_snapshot = ring_read_total;
    const auto used = write_start - read_snapshot;
    if (used >= ring_capacity_frames)
    {
        return 0U;
    }

    auto frames = source_bytes / sizeof(std::int16_t);
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

    std::memcpy(sample_ring.data() + ring_offset, source,
                frames * sizeof(std::int16_t));
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
    return frames * sizeof(std::int16_t);
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

void stop_codec_after_fault() noexcept
{
    auto& codec = m110::imxrt1170::wm8960_codec();
    if (codec.is_running())
    {
        (void)codec.stop();
    }
}

[[noreturn]] void suspend_player_task() noexcept
{
    for (;;)
    {
        vTaskSuspend(nullptr);
    }
}

void player_task_entry(void*) noexcept
{
    set_state(PlayerState::waiting_for_command);
    (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    if (!play_requested || !WFG_MediaIsLocal())
    {
        latch_fault(PlayerError::mount);
        suspend_player_task();
    }

    set_state(PlayerState::mounting);
    if (f_mount(&file_system, drive_path, 1U) != FR_OK)
    {
        latch_fault(PlayerError::mount);
        suspend_player_task();
    }
    file_system_owned = true;

    set_state(PlayerState::opening);
    if (f_open(&playback_file, playback_path, FA_READ) != FR_OK)
    {
        latch_fault(PlayerError::open);
        (void)f_mount(nullptr, drive_path, 0U);
        file_system_owned = false;
        suspend_player_task();
    }

    WavPayload payload{};
    if (!parse_wav(payload) || !seek_to(payload.offset))
    {
        if (!faulted)
        {
            latch_fault(PlayerError::seek);
        }
        (void)f_close(&playback_file);
        (void)f_mount(nullptr, drive_path, 0U);
        file_system_owned = false;
        suspend_player_task();
    }

    set_state(PlayerState::buffering);
    std::uint32_t bytes_remaining = payload.bytes;
    std::uint32_t bytes_staged{};
    std::uint32_t staged_offset{};
    bool codec_started{};

    while (!faulted)
    {
        if (bytes_staged != 0U)
        {
            const auto pushed = push_samples(read_buffer.data() + staged_offset,
                                             bytes_staged);
            if (pushed == 0U)
            {
                vTaskDelay(1U);
                continue;
            }
            staged_offset += pushed;
            bytes_staged -= pushed;
            if (!codec_started && ring_fill() >= prefill_frames)
            {
                codec_started = start_codec();
            }
            continue;
        }

        if (bytes_remaining == 0U)
        {
            __DMB();
            eof_enqueued = true;
            __DMB();
            if (!codec_started)
            {
                codec_started = start_codec();
            }
            break;
        }

        if (!read_payload_chunk(bytes_remaining, bytes_staged))
        {
            break;
        }
        staged_offset = 0U;
        bytes_remaining -= bytes_staged;
    }

    if (faulted)
    {
        stop_codec_after_fault();
    }

    (void)f_close(&playback_file);
    (void)f_mount(nullptr, drive_path, 0U);
    file_system_owned = false;
    while (!faulted && counters.state == PlayerState::playing)
    {
        vTaskDelay(pdMS_TO_TICKS(10U));
    }
    while (!faulted && counters.state == PlayerState::draining &&
           counters.eof_silence_frames < eof_drain_frames)
    {
        vTaskDelay(1U);
    }
    if (faulted)
    {
        stop_codec_after_fault();
    }
    if (!faulted && counters.state == PlayerState::draining)
    {
        (void)m110::imxrt1170::wm8960_codec().stop();
        taskENTER_CRITICAL();
        counters.pcm_drained = 1U;
        taskEXIT_CRITICAL();
        set_state(PlayerState::done);
    }
    suspend_player_task();
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
    taskENTER_CRITICAL();
    if (!WFG_MediaIsLocal())
    {
        taskEXIT_CRITICAL();
        return {m110::StatusCode::invalid_configuration, "media is not local"};
    }
    if (player_task_handle == nullptr ||
        counters.state != PlayerState::waiting_for_command || play_requested)
    {
        taskEXIT_CRITICAL();
        return {m110::StatusCode::busy, "player is not ready"};
    }
    play_requested = true;
    const auto task = player_task_handle;
    taskEXIT_CRITICAL();
    xTaskNotifyGive(task);
    return m110::Status::success();
}

bool player_allows_media_host() noexcept
{
    const bool codec_running = m110::imxrt1170::wm8960_codec().is_running();
    taskENTER_CRITICAL();
    const auto state = counters.state;
    const bool allowed =
        !file_system_owned &&
        !codec_running &&
        (state == PlayerState::stopped ||
         state == PlayerState::waiting_for_command ||
         state == PlayerState::done ||
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
    };
    taskEXIT_CRITICAL();
    return snapshot;
}

} // namespace waveform_generator
