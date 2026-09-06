#pragma once

#include <cstddef>

namespace waveform_generator
{

inline constexpr std::size_t cdc_response_capacity = 1536U;
inline constexpr std::size_t max_uint32_decimal_chars = 10U;

constexpr std::size_t literal_length(const char* value) noexcept
{
    std::size_t length = 0U;
    while (value[length] != '\0')
    {
        ++length;
    }
    return length;
}

constexpr std::size_t status_field_length(const char* name) noexcept
{
    return 1U + literal_length(name) + 1U + max_uint32_decimal_chars;
}

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
};

constexpr std::size_t status_response_worst_case_bytes() noexcept
{
    std::size_t total = literal_length("OK STATUS");
    for (const char* field : status_field_names)
    {
        total += status_field_length(field);
    }
    return total + 1U + 1U;
}

static_assert(status_response_worst_case_bytes() <= cdc_response_capacity);

} // namespace waveform_generator
