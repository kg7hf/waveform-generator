// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

#include "common/level.hpp"
#include "common/wav.hpp"
#include "rt1170/status_contract.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <vector>

namespace
{

using waveform_generator::WavPayload;
using waveform_generator::WavReader;
using waveform_generator::WavValidationError;

void append_u16(std::vector<std::uint8_t>& bytes, std::uint16_t value)
{
    bytes.push_back(static_cast<std::uint8_t>(value & 0xffU));
    bytes.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xffU));
}

void append_u32(std::vector<std::uint8_t>& bytes, std::uint32_t value)
{
    bytes.push_back(static_cast<std::uint8_t>(value & 0xffU));
    bytes.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xffU));
    bytes.push_back(static_cast<std::uint8_t>((value >> 16U) & 0xffU));
    bytes.push_back(static_cast<std::uint8_t>((value >> 24U) & 0xffU));
}

void patch_u16(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint16_t value)
{
    bytes[offset] = static_cast<std::uint8_t>(value & 0xffU);
    bytes[offset + 1U] = static_cast<std::uint8_t>((value >> 8U) & 0xffU);
}

void patch_u32(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value)
{
    bytes[offset] = static_cast<std::uint8_t>(value & 0xffU);
    bytes[offset + 1U] = static_cast<std::uint8_t>((value >> 8U) & 0xffU);
    bytes[offset + 2U] = static_cast<std::uint8_t>((value >> 16U) & 0xffU);
    bytes[offset + 3U] = static_cast<std::uint8_t>((value >> 24U) & 0xffU);
}

void append_fourcc(std::vector<std::uint8_t>& bytes, char a, char b, char c, char d)
{
    bytes.push_back(static_cast<std::uint8_t>(a));
    bytes.push_back(static_cast<std::uint8_t>(b));
    bytes.push_back(static_cast<std::uint8_t>(c));
    bytes.push_back(static_cast<std::uint8_t>(d));
}

void append_chunk_header(std::vector<std::uint8_t>& bytes, char a, char b, char c, char d, std::uint32_t size)
{
    append_fourcc(bytes, a, b, c, d);
    append_u32(bytes, size);
}

void append_fmt_chunk(std::vector<std::uint8_t>& bytes)
{
    append_chunk_header(bytes, 'f', 'm', 't', ' ', 16U);
    append_u16(bytes, 1U);
    append_u16(bytes, 1U);
    append_u32(bytes, 48000U);
    append_u32(bytes, 96000U);
    append_u16(bytes, 2U);
    append_u16(bytes, 16U);
}

void append_pcm_format_chunk(std::vector<std::uint8_t>& bytes, std::uint16_t channels, std::uint16_t bits_per_sample, std::uint32_t sample_rate, std::uint16_t block_align, std::uint32_t byte_rate)
{
    append_chunk_header(bytes, 'f', 'm', 't', ' ', 16U);
    append_u16(bytes, 1U);
    append_u16(bytes, channels);
    append_u32(bytes, sample_rate);
    append_u32(bytes, byte_rate);
    append_u16(bytes, block_align);
    append_u16(bytes, bits_per_sample);
}

void append_data_chunk(std::vector<std::uint8_t>& bytes, std::uint32_t samples)
{
    append_chunk_header(bytes, 'd', 'a', 't', 'a', samples * 2U);

    for (std::uint32_t index = 0U; index < samples; ++index)
    {
        append_u16(bytes, static_cast<std::uint16_t>(index));
    }
}

std::vector<std::uint8_t> make_riff()
{
    std::vector<std::uint8_t> bytes;
    append_fourcc(bytes, 'R', 'I', 'F', 'F');
    append_u32(bytes, 0U);
    append_fourcc(bytes, 'W', 'A', 'V', 'E');
    append_fmt_chunk(bytes);
    append_data_chunk(bytes, 4U);
    patch_u32(bytes, 4U, static_cast<std::uint32_t>(bytes.size() - 8U));
    return bytes;
}

std::vector<std::uint8_t> make_pcm_wav(std::uint16_t channels, std::uint16_t bits_per_sample, std::uint32_t sample_rate, std::uint16_t block_align, std::uint32_t byte_rate,
                                       std::span<const std::uint8_t> data, bool odd_ancillary = false)
{
    std::vector<std::uint8_t> bytes;
    append_fourcc(bytes, 'R', 'I', 'F', 'F');
    append_u32(bytes, 0U);
    append_fourcc(bytes, 'W', 'A', 'V', 'E');
    append_pcm_format_chunk(bytes, channels, bits_per_sample, sample_rate, block_align, byte_rate);

    if (odd_ancillary)
    {
        append_chunk_header(bytes, 'J', 'U', 'N', 'K', 1U);
        bytes.push_back(0x5aU);
        bytes.push_back(0U);
    }

    append_chunk_header(bytes, 'd', 'a', 't', 'a', static_cast<std::uint32_t>(data.size()));
    bytes.insert(bytes.end(), data.begin(), data.end());

    if ((data.size() & 1U) != 0U)
    {
        bytes.push_back(0U);
    }

    patch_u32(bytes, 4U, static_cast<std::uint32_t>(bytes.size() - 8U));
    return bytes;
}

// `count` bytes of `bytes` at `offset`: success, or wav_read_past_end.
m110::Status copy_bytes(const std::vector<std::uint8_t>& bytes, std::uint32_t offset, std::uint8_t* destination, std::uint32_t count) noexcept
{
    if (offset > bytes.size() || count > bytes.size() - offset)
    {
        return waveform_generator::wav_read_past_end;
    }

    std::ranges::copy(std::span{bytes}.subspan(offset, count), destination);
    return m110::Status::success();
}

m110::Status vector_read(void* context, std::uint32_t offset, std::uint8_t* destination, std::uint32_t count) noexcept
{
    return copy_bytes(*static_cast<const std::vector<std::uint8_t>*>(context), offset, destination, count);
}

struct ReadTrace
{
    const std::vector<std::uint8_t>* bytes{};
    std::uint32_t payload_offset{};
    std::uint32_t payload_bytes{};
    std::uint32_t calls{};
    std::uint32_t maximum_read{};
    bool touched_payload{};
};

m110::Status traced_read(void* context, std::uint32_t offset, std::uint8_t* destination, std::uint32_t count) noexcept
{
    auto* trace = static_cast<ReadTrace*>(context);
    ++trace->calls;
    trace->maximum_read = std::max(trace->maximum_read, count);
    const std::uint32_t end = offset + count;
    const std::uint32_t payload_end = trace->payload_offset + trace->payload_bytes;
    trace->touched_payload = trace->touched_payload || (offset < payload_end && trace->payload_offset < end);
    return copy_bytes(*trace->bytes, offset, destination, count);
}

WavValidationError validate(std::vector<std::uint8_t>& bytes, WavPayload& payload)
{
    const WavReader reader{
        .context = &bytes,
        .file_size = static_cast<std::uint32_t>(bytes.size()),
        .read = &vector_read,
    };
    return waveform_generator::validate_pcm16_mono_48k_wav(reader, payload);
}

WavValidationError validate_stereo(std::vector<std::uint8_t>& bytes, WavPayload& payload)
{
    const WavReader reader{
        .context = &bytes,
        .file_size = static_cast<std::uint32_t>(bytes.size()),
        .read = &vector_read,
    };
    return waveform_generator::validate_pcm24_stereo_48k_wav(reader, payload);
}

// A reader over `bytes` that counts its calls and fails the call numbered
// `failing` (0: the first) with an io_error of its own.
struct CountingReader
{
    const std::vector<std::uint8_t>* bytes{};
    std::uint32_t failing{std::numeric_limits<std::uint32_t>::max()};
    std::uint32_t calls{};
};

constexpr m110::Status card_failure{.code = m110::StatusCode::io_error, .message = "the test card failed"};

m110::Status counting_read(void* context, std::uint32_t offset, std::uint8_t* destination, std::uint32_t count) noexcept
{
    auto* reader = static_cast<CountingReader*>(context);

    if (reader->calls++ == reader->failing)
    {
        return card_failure;
    }

    return copy_bytes(*reader->bytes, offset, destination, count);
}

bool same_status(m110::Status left, m110::Status right)
{
    return left.code == right.code && std::strcmp(left.message, right.message) == 0;
}

// The exit codes of run_wav_read_tests()' checks.
enum WavReadCheck : std::uint8_t
{
    read_to_the_end = 20,
    read_past_the_end,
    reader_failure_passed_on,
    reader_missing,
    header_read_failure,
    chunk_read_failure,
    whole_file_read,
};

// wav_read_exact() tells the end of the file from a failed read: a read past
// the end is wav_read_past_end and the callback is not asked for it; the
// callback's failure is passed on as it is.
int run_wav_read_tests()
{
    const auto bytes = make_riff();
    const auto size = static_cast<std::uint32_t>(bytes.size());
    constexpr std::uint32_t tail = 4U;
    constexpr std::uint32_t data_bytes = 8U;
    CountingReader counting{.bytes = &bytes};
    const WavReader reader{.context = &counting, .file_size = size, .read = &counting_read};
    std::array<std::uint8_t, tail + 1U> destination{};
    const std::span<std::uint8_t> last_bytes = std::span{destination}.first(tail);

    if (!waveform_generator::wav_read_exact(reader, size - tail, last_bytes).is_ok() || counting.calls != 1U)
    {
        return read_to_the_end;
    }

    if (!same_status(waveform_generator::wav_read_exact(reader, size - tail, destination), waveform_generator::wav_read_past_end) ||
        !same_status(waveform_generator::wav_read_exact(reader, size + 1U, last_bytes.first(0U)), waveform_generator::wav_read_past_end) ||
        !same_status(waveform_generator::wav_read_exact(reader, std::numeric_limits<std::uint32_t>::max(), last_bytes), waveform_generator::wav_read_past_end) || counting.calls != 1U)
    {
        return read_past_the_end;
    }

    counting.failing = counting.calls;

    if (!same_status(waveform_generator::wav_read_exact(reader, 0U, last_bytes), card_failure) || counting.calls != 2U)
    {
        return reader_failure_passed_on;
    }

    const WavReader without_callback{.context = &counting, .file_size = size, .read = nullptr};

    if (!same_status(waveform_generator::wav_read_exact(without_callback, 0U, last_bytes), waveform_generator::wav_read_unready) ||
        !same_status(waveform_generator::wav_read_exact(reader, 0U, std::span<std::uint8_t>{}), waveform_generator::wav_read_unready) || counting.calls != 2U)
    {
        return reader_missing;
    }

    // A failed read keeps the validation's own errors: io for the RIFF
    // header, invalid_riff for a chunk header.
    WavPayload payload{};
    counting = {.bytes = &bytes, .failing = 0U};

    if (waveform_generator::validate_pcm16_mono_48k_wav(reader, payload) != WavValidationError::io)
    {
        return header_read_failure;
    }

    counting = {.bytes = &bytes, .failing = 1U};

    if (waveform_generator::validate_pcm16_mono_48k_wav(reader, payload) != WavValidationError::invalid_riff)
    {
        return chunk_read_failure;
    }

    counting = {.bytes = &bytes};

    if (waveform_generator::validate_pcm16_mono_48k_wav(reader, payload) != WavValidationError::none || payload.bytes != data_bytes)
    {
        return whole_file_read;
    }

    return 0;
}

int run_wav_tests()
{
    {
        auto bytes = make_riff();
        append_chunk_header(bytes, 'J', 'U', 'N', 'K', 2U);
        bytes.push_back(1U);
        bytes.push_back(2U);
        patch_u32(bytes, 4U, static_cast<std::uint32_t>(bytes.size() - 8U));
        WavPayload payload{};

        if (validate(bytes, payload) != WavValidationError::none || payload.offset != 44U || payload.bytes != 8U)
        {
            return 10;
        }
    }

    {
        auto bytes = make_riff();
        patch_u32(bytes, 4U, static_cast<std::uint32_t>(bytes.size() - 7U));
        WavPayload payload{};

        if (validate(bytes, payload) != WavValidationError::invalid_riff)
        {
            return 11;
        }
    }

    {
        auto bytes = make_riff();
        append_fmt_chunk(bytes);
        patch_u32(bytes, 4U, static_cast<std::uint32_t>(bytes.size() - 8U));
        WavPayload payload{};

        if (validate(bytes, payload) != WavValidationError::invalid_riff)
        {
            return 12;
        }
    }

    {
        auto bytes = make_riff();
        append_data_chunk(bytes, 1U);
        patch_u32(bytes, 4U, static_cast<std::uint32_t>(bytes.size() - 8U));
        WavPayload payload{};

        if (validate(bytes, payload) != WavValidationError::invalid_riff)
        {
            return 13;
        }
    }

    {
        auto bytes = make_riff();
        append_chunk_header(bytes, 'J', 'U', 'N', 'K', 4U);
        bytes.push_back(0U);
        patch_u32(bytes, 4U, static_cast<std::uint32_t>(bytes.size() - 8U));
        WavPayload payload{};

        if (validate(bytes, payload) != WavValidationError::invalid_riff)
        {
            return 14;
        }
    }

    {
        std::vector<std::uint8_t> bytes;
        append_fourcc(bytes, 'R', 'I', 'F', 'F');
        append_u32(bytes, 0U);
        append_fourcc(bytes, 'W', 'A', 'V', 'E');
        append_fmt_chunk(bytes);
        append_chunk_header(bytes, 'J', 'U', 'N', 'K', 65500U);
        bytes.resize(bytes.size() + 65500U);
        append_data_chunk(bytes, 1U);
        patch_u32(bytes, 4U, static_cast<std::uint32_t>(bytes.size() - 8U));
        WavPayload payload{};

        if (validate(bytes, payload) != WavValidationError::invalid_riff)
        {
            return 15;
        }
    }

    return 0;
}

int run_stereo_wav_tests()
{
    const std::array<std::uint8_t, 6U> interleaved{0x03U, 0x02U, 0x01U, 0xC0U, 0xB0U, 0xA0U};
    {
        const std::array<std::uint8_t, 6U> mono_data{};
        auto bytes = make_pcm_wav(1U, 24U, 48000U, 3U, 144000U, mono_data);
        const WavReader reader{.context = &bytes, .file_size = static_cast<std::uint32_t>(bytes.size()), .read = &vector_read};
        WavPayload payload{};

        if (waveform_generator::validate_pcm24_mono_48k_wav(reader, payload) != WavValidationError::none || payload.bits_per_sample != 24U || payload.bytes_per_frame != 3U)
        {
            return 41;
        }
    }

    {
        auto bytes = make_pcm_wav(2U, 24U, 48000U, 6U, 288000U, interleaved, true);
        constexpr std::uint32_t data_offset = 54U;
        ReadTrace trace{.bytes = &bytes, .payload_offset = data_offset, .payload_bytes = 6U};
        const WavReader reader{.context = &trace, .file_size = static_cast<std::uint32_t>(bytes.size()), .read = &traced_read};
        WavPayload payload{};

        if (waveform_generator::validate_pcm24_stereo_48k_wav(reader, payload) != WavValidationError::none || payload.offset != data_offset || payload.bytes != interleaved.size() ||
            payload.bits_per_sample != 24U || payload.bytes_per_frame != 6U || trace.touched_payload || trace.calls != 5U || trace.maximum_read > 16U)
        {
            return 30;
        }

        std::array<std::uint8_t, 6U> decoded{};

        if (!waveform_generator::wav_read_exact(reader, payload.offset, decoded).is_ok() || decoded != interleaved)
        {
            return 31;
        }
    }

    {
        auto bytes = make_pcm_wav(2U, 24U, 48000U, 6U, 288000U, interleaved);
        WavPayload payload{};

        if (validate(bytes, payload) != WavValidationError::unsupported_format)
        {
            return 32;
        }
    }

    {
        const std::array<std::uint8_t, 3U> mono_data{};
        auto bytes = make_pcm_wav(1U, 24U, 48000U, 3U, 144000U, mono_data);
        WavPayload payload{};
        const WavReader reader{.context = &bytes, .file_size = static_cast<std::uint32_t>(bytes.size()), .read = &vector_read};

        if (waveform_generator::validate_pcm24_stereo_48k_wav(reader, payload) != WavValidationError::unsupported_format)
        {
            return 33;
        }
    }

    {
        const std::array<std::uint8_t, 8U> data{};
        std::array pcm_cases{
            make_pcm_wav(2U, 16U, 48000U, 4U, 192000U, data),
            make_pcm_wav(2U, 32U, 48000U, 8U, 384000U, data),
            make_pcm_wav(2U, 24U, 44100U, 6U, 288000U, interleaved),
        };

        for (auto& bytes : pcm_cases)
        {
            WavPayload payload{};
            const WavReader reader{.context = &bytes, .file_size = static_cast<std::uint32_t>(bytes.size()), .read = &vector_read};

            if (waveform_generator::validate_pcm24_stereo_48k_wav(reader, payload) != WavValidationError::unsupported_format)
            {
                return 34;
            }
        }
    }

    {
        auto bytes = make_pcm_wav(2U, 24U, 48000U, 6U, 288000U, interleaved);
        patch_u16(bytes, 32U, 4U);
        WavPayload payload{};

        if (validate_stereo(bytes, payload) != WavValidationError::unsupported_format)
        {
            return 35;
        }

        patch_u16(bytes, 32U, 6U);
        patch_u32(bytes, 28U, 288001U);

        if (validate_stereo(bytes, payload) != WavValidationError::unsupported_format)
        {
            return 36;
        }
    }

    {
        const std::array<std::uint8_t, 5U> misaligned{};
        auto bytes = make_pcm_wav(2U, 24U, 48000U, 6U, 288000U, misaligned);
        WavPayload payload{};

        if (validate_stereo(bytes, payload) != WavValidationError::unsupported_format)
        {
            return 37;
        }

        patch_u32(bytes, 40U, 7U);

        if (validate_stereo(bytes, payload) != WavValidationError::invalid_riff)
        {
            return 38;
        }
    }

    {
        auto bytes = make_pcm_wav(2U, 24U, 48000U, 6U, 288000U, interleaved);
        append_pcm_format_chunk(bytes, 2U, 24U, 48000U, 6U, 288000U);
        patch_u32(bytes, 4U, static_cast<std::uint32_t>(bytes.size() - 8U));
        WavPayload payload{};

        if (validate_stereo(bytes, payload) != WavValidationError::invalid_riff)
        {
            return 39;
        }

        bytes = make_pcm_wav(2U, 24U, 48000U, 6U, 288000U, interleaved);
        append_chunk_header(bytes, 'd', 'a', 't', 'a', static_cast<std::uint32_t>(interleaved.size()));
        bytes.insert(bytes.end(), interleaved.begin(), interleaved.end());
        patch_u32(bytes, 4U, static_cast<std::uint32_t>(bytes.size() - 8U));

        if (validate_stereo(bytes, payload) != WavValidationError::invalid_riff)
        {
            return 40;
        }
    }

    return 0;
}

} // namespace

int main()
{
    using waveform_generator::headphone_percent_to_raw;
    static_assert(waveform_generator::status_response_worst_case_bytes() <= waveform_generator::cdc_response_capacity);
    // 1465 bytes for the field set, 413 bytes for the fourteen engine_* fields,
    // 294 bytes for the nine output-FIFO / producer fields, 106 bytes for the three
    // audio TX-race / SAI FIFO-residue fields, 68 bytes for the three build-identity
    // fields (fw_commit_hi, fw_commit_lo, fw_dirty), 47 bytes for output_route and
    // msc_ejected, 25 bytes for msc_host_hold, 544 bytes for the seventeen USB
    // sound-card fields (uac_stream_open .. uac_play_first_miss_frame_index; 80 of
    // them uac_play_missed_intervals and uac_play_first_miss_frame_index).
    constexpr std::size_t text_status_bytes = 2962U;
    static_assert(waveform_generator::status_response_worst_case_bytes() == text_status_bytes);
    static_assert(waveform_generator::status_json_worst_case_bytes() <= waveform_generator::cdc_response_capacity);
    // JSON STATUS?/COUNTERS?: 158 bytes of prefix and object header, 3159 bytes for
    // the shared numeric fields (51 of them output_route and msc_ejected, 27
    // msc_host_hold, 578 the seventeen USB sound-card fields, 84 of those
    // uac_play_missed_intervals and uac_play_first_miss_frame_index), 768 bytes
    // for the JSON-only members (including the build_id and fw_commit strings),
    // "}}", newline and terminator: 7 bytes below the 4096-byte response buffer.
    constexpr std::size_t json_status_bytes = 4089U;
    static_assert(waveform_generator::status_json_worst_case_bytes() == json_status_bytes);
    // USB?: the same 114-byte prefix, 34 bytes of "usb" object header, 1293
    // bytes for the thirty-nine fields (eight stream diagnostics as STATUS
    // reports them, uac_rejected_chunks, the six stream miss-attribution
    // fields, uac_play_unprimed_intervals, 170 bytes for the five TinyUSB
    // assert fields usb_assert_count .. usb_last_assert_tick, 162 bytes for
    // the five capture-format fields uac_stream_alt .. uac_play_stream_channels,
    // 96 bytes for the three MSC transfer fields msc_max_write_us ..
    // msc_max_blocks_per_call, 29 bytes for sd_bus_clock_hz, 215 bytes for
    // the six CUT fields uac_play_cut_count .. uac_play_sent_frames, 36
    // bytes for engine_deadline_misses, 62 for the EP0 order's two counters
    // usb_ep0_reordered and usb_ep0_unordered, and 33 for
    // uac_sent_mismatches), "}}", newline and terminator. New USB diagnostics
    // grow this reply, never STATUS.
    constexpr std::size_t json_usb_bytes = 1481U;
    static_assert(waveform_generator::usb_json_worst_case_bytes() == json_usb_bytes);
    // ENGINE?: the same 114-byte prefix, 40 bytes of "engine" object header,
    // 26 bytes each for engine_state and engine_error, 275 for engine_reason
    // (128 characters, each escaped, in quotes), 164 for the channel identity,
    // "}}", newline and terminator.
    constexpr std::size_t json_engine_bytes = 984U;
    static_assert(waveform_generator::engine_json_worst_case_bytes() == json_engine_bytes);
    // PLAYLIST?: the same prefix, the progress (18 bytes of it for the
    // tick), and two result blocks of 1740 bytes each at their widest.
    // Channel/engine digests add 224 bytes and second-source identity 335; a third would
    // exceed the 4096-byte response limit (status_contract.hpp).
    constexpr std::size_t json_playlist_bytes = 3859U;
    constexpr std::size_t playlist_block_bytes = 1740U;
    static_assert(waveform_generator::playlist_json_worst_case_bytes() == json_playlist_bytes && waveform_generator::playlist_block_worst_case_bytes() == playlist_block_bytes);
    constexpr std::array<std::uint8_t, 6> inputs{0U, 1U, 2U, 50U, 70U, 100U};
    constexpr std::array<std::uint8_t, 6> expected{0x00U, 0x30U, 0x31U, 0x57U, 0x67U, 0x7fU};

    for (std::size_t index = 0; index < inputs.size(); ++index)
    {
        if (headphone_percent_to_raw(inputs[index]) != expected[index])
        {
            return 1;
        }
    }

    std::uint8_t previous = headphone_percent_to_raw(1U);

    for (std::uint8_t percent = 2U; percent <= 100U; ++percent)
    {
        const auto current = headphone_percent_to_raw(percent);

        if (current < previous || current < 0x30U || current > 0x7fU)
        {
            return 2;
        }

        previous = current;
    }

    if (const int wav = run_wav_tests(); wav != 0)
    {
        return wav;
    }

    if (const int stereo_wav = run_stereo_wav_tests(); stereo_wav != 0)
    {
        return stereo_wav;
    }

    return run_wav_read_tests();
}
