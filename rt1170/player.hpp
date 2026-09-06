#pragma once

#include "common/status.hpp"

#include <cstdint>

namespace waveform_generator
{

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
};

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
    std::uint32_t sd_bytes_read;
    std::uint32_t sd_short_reads;
    std::uint32_t sd_read_errors;
    std::uint32_t underruns;
    std::uint32_t first_underrun_frame;
    std::uint32_t max_sd_read_cycles;
};

/*
 * Start the P1.2 engineering slice in an idle state. After the USB owner grants
 * local/FatFs ownership, request_player_play() opens 2:/WG/PLAY.WAV, accepts
 * PCM16/48 kHz/mono, prebuffers, then plays it once. Submitted frames are
 * copied into the SAI/eDMA buffer from the audio callback; done is not entered
 * until a bounded post-EOF silence drain has completed. This temporary control
 * surface predates WFG/1 and does not produce a qualified artifact.
 */
[[nodiscard]] m110::Status start_player_checkpoint() noexcept;
[[nodiscard]] m110::Status request_player_play() noexcept;
[[nodiscard]] bool player_allows_media_host() noexcept;
[[nodiscard]] PlayerSnapshot player_snapshot() noexcept;

} // namespace waveform_generator
