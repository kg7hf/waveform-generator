#include "platform/board.hpp"
#include "platform/runtime.hpp"

#if defined(WFG_TONE_IMAGE)
#include "usb_checkpoint.hpp"
#include "tone.hpp"
#elif defined(WFG_PLAYER_IMAGE)
#include "usb_checkpoint.hpp"
#include "player.hpp"
#include "platform/waveform_msc.h"
#elif defined(WFG_SDCARD_IMAGE)
#include "sdcard_probe.hpp"
#else
#include "usb_checkpoint.hpp"
#endif
#if !defined(WFG_SDCARD_IMAGE)
#include "platform/wm8960_codec.hpp"
#endif

extern "C"
{
#include "FreeRTOS.h"
#include "task.h"
}

#include <cstdint>

#ifndef WFG_BUILD_ID
#error WFG_BUILD_ID must be supplied by the local build
#endif

struct WaveformCheckpointStatus
{
    std::uint32_t signature;
    std::uint32_t image;
    std::uint32_t setup_status;
    std::uint32_t heartbeats;
    std::uint32_t audio_blocks;
    std::uint32_t audio_overruns;
    std::uint32_t audio_rx_errors;
    std::uint32_t audio_tx_errors;
    std::uint32_t audio_max_backlog_blocks;
    std::uint32_t audio_max_block_cycles;
#if defined(WFG_PLAYER_IMAGE)
    std::uint32_t player_state;
    std::uint32_t player_error;
    std::uint32_t player_data_bytes;
    std::uint32_t player_file_frames_total;
    std::uint32_t player_file_frames_enqueued;
    std::uint32_t player_ring_fill_frames;
    std::uint32_t player_ring_min_frames;
    std::uint32_t player_ring_min_pre_eof_frames;
    std::uint32_t player_ring_max_frames;
    std::uint32_t player_frames_requested;
    std::uint32_t player_file_frames_submitted;
    std::uint32_t player_pcm_drained;
    std::uint32_t player_eof_count;
    std::uint32_t player_eof_silence_frames;
    std::uint32_t player_sd_read_calls;
    std::uint32_t player_sd_bytes_read;
    std::uint32_t player_sd_short_reads;
    std::uint32_t player_sd_read_errors;
    std::uint32_t player_underruns;
    std::uint32_t player_first_underrun_frame;
    std::uint32_t player_max_sd_read_cycles;
    std::uint32_t media_state;
    std::uint32_t media_error;
    std::uint32_t media_card_ready;
    std::uint32_t media_msc_ready;
    std::uint32_t media_msc_read_only;
    std::uint32_t media_block_count;
    std::uint32_t media_block_size;
    std::uint32_t media_init_attempts;
    std::uint32_t media_host_init_status;
    std::uint32_t media_host_detect_status;
    std::uint32_t media_card_init_status;
    std::uint32_t media_disk_init_status;
    std::uint32_t media_test_ready_calls;
    std::uint32_t media_not_ready_responses;
    std::uint32_t media_read_calls;
    std::uint32_t media_read_blocks;
    std::uint32_t media_write_calls;
    std::uint32_t media_write_blocks;
    std::uint32_t media_read_errors;
    std::uint32_t media_write_errors;
    std::uint32_t media_invalid_requests;
    std::uint32_t media_eject_requests;
    std::uint32_t media_sync_cache_calls;
    std::uint32_t media_ownership_handoffs;
    std::uint32_t media_cd_gpio3_level;
    std::uint32_t media_cd_cm7_gpio3_level;
    std::uint32_t media_cd_inserted;
#endif
};

extern "C"
{
    volatile WaveformCheckpointStatus g_waveform_checkpoint{};
}

namespace waveform_generator
{

int application_main() noexcept
{
    g_waveform_checkpoint.signature = 0x57464731U;
    g_waveform_checkpoint.setup_status = 1U;
#if defined(WFG_PLAYER_IMAGE)
    m110::platform_write_diagnostic("RT1170 waveform generator P1.2 microSD checkpoint\r\n");
#elif defined(WFG_SDCARD_IMAGE)
    m110::platform_write_diagnostic("RT1170 waveform generator SD-only bring-up checkpoint\r\n");
#else
    m110::platform_write_diagnostic("RT1170 waveform generator P1.1 build checkpoint\r\n");
#endif
    m110::platform_write_diagnostic("Build: " WFG_BUILD_ID "\r\n");
#if defined(WFG_PLAYER_IMAGE)
    m110::platform_write_diagnostic("WFG/1 control and source qualification are unavailable.\r\n");
#elif defined(WFG_SDCARD_IMAGE)
    m110::platform_write_diagnostic("USB, WFG/1 control, FatFs writes, and formatting are unavailable.\r\n");
#else
    m110::platform_write_diagnostic("WFG/1 control, SD playback, and source qualification are unavailable.\r\n");
#endif
#if !defined(WFG_SDCARD_IMAGE)
    const auto usb_status = start_usb_checkpoint();
    if (!usb_status.is_ok())
    {
        g_waveform_checkpoint.setup_status = 0xE001U;
        return 1;
    }
#endif
#if defined(WFG_PLAYER_IMAGE)
    if (!WFG_MediaStartInitialization())
    {
        m110::platform_write_diagnostic(
            "microSD initialization task did not start; CDC STATUS remains available.\r\n");
    }
#endif

#if defined(WFG_TONE_IMAGE)
    g_waveform_checkpoint.image = 1U;
    m110::platform_write_diagnostic("Image: nominal 1 kHz tone, unqualified hardware checkpoint.\r\n");
    const auto tone_status = start_tone_checkpoint();
    if (!tone_status.is_ok())
    {
        g_waveform_checkpoint.setup_status = 0xE002U;
        return 1;
    }
#elif defined(WFG_PLAYER_IMAGE)
    g_waveform_checkpoint.image = 2U;
    m110::platform_write_diagnostic(
        "Image: CDC-commanded 2:/WG/PLAY.WAV, one pass, engineering checkpoint.\r\n");
    const auto player_status = start_player_checkpoint();
    if (!player_status.is_ok())
    {
        g_waveform_checkpoint.setup_status = 0xE003U;
        return 1;
    }
#elif defined(WFG_SDCARD_IMAGE)
    g_waveform_checkpoint.image = 3U;
    const auto sd_status = run_sdcard_probe();
    if (!sd_status.is_ok())
    {
        m110::platform_write_diagnostic("SD-only bring-up failed: ");
        m110::platform_write_diagnostic(sd_status.message);
        m110::platform_write_diagnostic("\r\n");
        g_waveform_checkpoint.setup_status = 0xE004U;
        return 1;
    }
#else
    g_waveform_checkpoint.image = 0U;
    m110::platform_write_diagnostic("Image: scaffold, audio stopped.\r\n");
#endif

    g_waveform_checkpoint.setup_status = 2U;
    for (;;)
    {
#if !defined(WFG_SDCARD_IMAGE)
        const auto& codec = m110::imxrt1170::wm8960_codec();
        g_waveform_checkpoint.audio_blocks = codec.blocks_processed();
        g_waveform_checkpoint.audio_overruns = codec.rx_overruns();
        g_waveform_checkpoint.audio_rx_errors = codec.rx_errors();
        g_waveform_checkpoint.audio_tx_errors = codec.tx_errors();
        g_waveform_checkpoint.audio_max_backlog_blocks =
            codec.max_backlog_blocks();
        g_waveform_checkpoint.audio_max_block_cycles = codec.max_block_cycles();
#endif
#if defined(WFG_PLAYER_IMAGE)
        const auto player = player_snapshot();
        g_waveform_checkpoint.player_state = player.state;
        g_waveform_checkpoint.player_error = player.error;
        g_waveform_checkpoint.player_data_bytes = player.data_bytes;
        g_waveform_checkpoint.player_file_frames_total = player.file_frames_total;
        g_waveform_checkpoint.player_file_frames_enqueued = player.file_frames_enqueued;
        g_waveform_checkpoint.player_ring_fill_frames = player.ring_fill_frames;
        g_waveform_checkpoint.player_ring_min_frames = player.ring_min_frames;
        g_waveform_checkpoint.player_ring_min_pre_eof_frames =
            player.ring_min_pre_eof_frames;
        g_waveform_checkpoint.player_ring_max_frames = player.ring_max_frames;
        g_waveform_checkpoint.player_frames_requested = player.frames_requested;
        g_waveform_checkpoint.player_file_frames_submitted =
            player.file_frames_submitted;
        g_waveform_checkpoint.player_pcm_drained = player.pcm_drained;
        g_waveform_checkpoint.player_eof_count = player.eof_count;
        g_waveform_checkpoint.player_eof_silence_frames = player.eof_silence_frames;
        g_waveform_checkpoint.player_sd_read_calls = player.sd_read_calls;
        g_waveform_checkpoint.player_sd_bytes_read = player.sd_bytes_read;
        g_waveform_checkpoint.player_sd_short_reads = player.sd_short_reads;
        g_waveform_checkpoint.player_sd_read_errors = player.sd_read_errors;
        g_waveform_checkpoint.player_underruns = player.underruns;
        g_waveform_checkpoint.player_first_underrun_frame = player.first_underrun_frame;
        g_waveform_checkpoint.player_max_sd_read_cycles = player.max_sd_read_cycles;
        const auto media = WFG_MediaGetSnapshot();
        g_waveform_checkpoint.media_state = media.state;
        g_waveform_checkpoint.media_error = media.error;
        g_waveform_checkpoint.media_card_ready = media.card_ready;
        g_waveform_checkpoint.media_msc_ready = media.msc_ready;
        g_waveform_checkpoint.media_msc_read_only = media.msc_read_only;
        g_waveform_checkpoint.media_block_count = media.block_count;
        g_waveform_checkpoint.media_block_size = media.block_size;
        g_waveform_checkpoint.media_init_attempts = media.init_attempts;
        g_waveform_checkpoint.media_host_init_status = media.host_init_status;
        g_waveform_checkpoint.media_host_detect_status = media.host_detect_status;
        g_waveform_checkpoint.media_card_init_status = media.card_init_status;
        g_waveform_checkpoint.media_disk_init_status = media.disk_init_status;
        g_waveform_checkpoint.media_test_ready_calls = media.test_ready_calls;
        g_waveform_checkpoint.media_not_ready_responses = media.not_ready_responses;
        g_waveform_checkpoint.media_read_calls = media.read_calls;
        g_waveform_checkpoint.media_read_blocks = media.read_blocks;
        g_waveform_checkpoint.media_write_calls = media.write_calls;
        g_waveform_checkpoint.media_write_blocks = media.write_blocks;
        g_waveform_checkpoint.media_read_errors = media.read_errors;
        g_waveform_checkpoint.media_write_errors = media.write_errors;
        g_waveform_checkpoint.media_invalid_requests = media.invalid_requests;
        g_waveform_checkpoint.media_eject_requests = media.eject_requests;
        g_waveform_checkpoint.media_sync_cache_calls = media.sync_cache_calls;
        g_waveform_checkpoint.media_ownership_handoffs = media.ownership_handoffs;
        g_waveform_checkpoint.media_cd_gpio3_level = media.cd_gpio3_level;
        g_waveform_checkpoint.media_cd_cm7_gpio3_level = media.cd_cm7_gpio3_level;
        g_waveform_checkpoint.media_cd_inserted = media.cd_inserted;
#endif
        g_waveform_checkpoint.heartbeats = g_waveform_checkpoint.heartbeats + 1U;
        m110::platform_set_status(m110::PlatformStatus::running);
        vTaskDelay(pdMS_TO_TICKS(250U));
        m110::platform_set_status(m110::PlatformStatus::idle);
        vTaskDelay(pdMS_TO_TICKS(250U));
    }
}

} // namespace waveform_generator

int main()
{
    return m110::run_application(&waveform_generator::application_main);
}
