// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// In-memory FatFs contract tests for the RT1170 secondary WAV provider.

#include "rt1170/secondary_wav.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace
{
using namespace waveform_generator;

int failures{};
std::vector<std::uint8_t> file_bytes;
std::string expected_path{"2:/WG/TEST.WAV"};
std::uint64_t read_calls{};
std::uint64_t close_calls{};
std::uint64_t open_handles{};
std::uint32_t payload_offset{};
std::uint32_t maximum_read{};
bool fail_open{};
bool fail_close{};
bool short_next_read{};
bool fail_next_read{};
bool fail_next_seek{};
bool all_alignment_matches{true};
std::uint8_t alignment_offsets_mask{};

void check(bool condition, const char* message)
{
    if (!condition)
    {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

void put_u16(std::vector<std::uint8_t>& bytes, std::uint16_t value)
{
    bytes.push_back(static_cast<std::uint8_t>(value));
    bytes.push_back(static_cast<std::uint8_t>(value >> 8U));
}

void put_u32(std::vector<std::uint8_t>& bytes, std::uint32_t value)
{
    for (std::uint32_t shift = 0U; shift < 32U; shift += 8U)
    {
        bytes.push_back(static_cast<std::uint8_t>(value >> shift));
    }
}

void set_u32(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value)
{
    for (std::uint32_t shift = 0U; shift < 32U; shift += 8U)
    {
        bytes[offset + shift / 8U] = static_cast<std::uint8_t>(value >> shift);
    }
}

void append_fourcc(std::vector<std::uint8_t>& bytes, const char* text)
{
    for (std::size_t index = 0U; index < 4U; ++index)
    {
        bytes.push_back(static_cast<std::uint8_t>(text[index]));
    }
}

std::vector<std::uint8_t> pcm24_wav(const std::vector<audio::Pcm24Sample>& samples, std::uint16_t channels = 1U, std::uint16_t bits = 24U, std::uint8_t junk_size = 0U)
{
    const std::uint32_t bytes_per_frame = static_cast<std::uint32_t>(channels) * bits / 8U;
    std::vector<std::uint8_t> result;
    append_fourcc(result, "RIFF");
    put_u32(result, 0U);
    append_fourcc(result, "WAVE");
    append_fourcc(result, "fmt ");
    put_u32(result, 16U);
    put_u16(result, 1U);
    put_u16(result, channels);
    put_u32(result, 48000U);
    put_u32(result, 48000U * bytes_per_frame);
    put_u16(result, static_cast<std::uint16_t>(bytes_per_frame));
    put_u16(result, bits);

    if (junk_size != 0U)
    {
        append_fourcc(result, "JUNK");
        put_u32(result, junk_size);

        for (std::uint8_t index = 0U; index < junk_size; ++index)
        {
            result.push_back(static_cast<std::uint8_t>(0xA0U + index));
        }

        if ((junk_size & 1U) != 0U)
        {
            result.push_back(0U);
        }
    }

    append_fourcc(result, "data");
    put_u32(result, static_cast<std::uint32_t>(samples.size()) * bytes_per_frame);
    payload_offset = static_cast<std::uint32_t>(result.size());

    for (const auto sample : samples)
    {
        for (std::uint32_t channel = 0U; channel < channels; ++channel)
        {
            if (bits == 16U)
            {
                const auto pcm16 = static_cast<std::int16_t>(sample >> 8U);
                put_u16(result, static_cast<std::uint16_t>(pcm16));
            }
            else
            {
                std::array<std::uint8_t, 3U> packed{};
                audio::write_pcm24_le(packed.data(), sample);
                result.insert(result.end(), packed.begin(), packed.end());
            }
        }
    }

    set_u32(result, 4U, static_cast<std::uint32_t>(result.size() - 8U));
    return result;
}

void reset_fake()
{
    file_bytes.clear();
    expected_path = "2:/WG/TEST.WAV";
    read_calls = 0U;
    close_calls = 0U;
    open_handles = 0U;
    payload_offset = 0U;
    maximum_read = 0U;
    fail_open = false;
    fail_close = false;
    short_next_read = false;
    fail_next_read = false;
    fail_next_seek = false;
    all_alignment_matches = true;
    alignment_offsets_mask = 0U;
}

std::vector<audio::Pcm24Sample> test_samples(std::size_t count)
{
    std::vector<audio::Pcm24Sample> values(count);

    for (std::size_t index = 0U; index < count; ++index)
    {
        const auto phase = static_cast<std::int32_t>((index * 7919U) % 0x007FFFF0U);
        values[index] = index % 11U == 0U ? 0 : (index % 2U == 0U ? phase : -phase);
    }

    return values;
}

std::uint64_t digest_pcm24(const std::vector<audio::Pcm24Sample>& samples, std::size_t first, std::size_t count)
{
    std::uint64_t hash = 0xCBF29CE484222325ULL;

    for (std::size_t index = first; index < first + count; ++index)
    {
        std::array<std::uint8_t, 3U> bytes{};
        audio::write_pcm24_le(bytes.data(), samples[index]);

        for (const auto byte : bytes)
        {
            hash ^= byte;
            hash *= 0x100000001B3ULL;
        }
    }

    return hash;
}

double rms_pcm24(const std::vector<audio::Pcm24Sample>& samples, std::size_t first, std::size_t count)
{
    double energy{};

    for (std::size_t index = first; index < first + count; ++index)
    {
        const double sample = audio::pcm24_to_float(samples[index]);
        energy += sample * sample;
    }

    return std::sqrt(energy / static_cast<double>(count));
}

signal_lab::SecondSourceParams excerpt_params(std::uint64_t start, std::uint64_t count)
{
    signal_lab::SecondSourceParams params{};
    params.source_start_frame = start;
    params.duration_frames = count;
    return params;
}

void pcm24_measurement_and_reads()
{
    reset_fake();
    const auto samples = test_samples(6200U);
    file_bytes = pcm24_wav(samples, 1U, 24U, 2U);
    expected_path = "2:/WG/TEST.WAV";
    Rt1170SecondaryWav provider;
    check(provider.read(0U, {}).code == m110::StatusCode::invalid_configuration, "read is refused before the source excerpt has been measured");
    check(provider.open("TEST.WAV", excerpt_params(1U, 5000U)).is_ok(), "valid PCM24 WAV opens with an excerpt");
    check(provider.frames() == samples.size() && provider.selected_start() == 1U && provider.selected_frames() == 5000U, "source and selected excerpt metadata are exact");
    std::array<float, 2048U> scratch{};
    const std::array<std::size_t, 3U> sizes{1U, 127U, 2048U};
    std::size_t block{};

    for (;;)
    {
        const std::size_t count = sizes[block++ % sizes.size()];
        auto measured = provider.measure_step(std::span{scratch}.first(count));
        check(measured.has_value(), "bounded measurement chunk succeeds");

        if (!measured.has_value() || measured.value() == SecondaryMeasureProgress::complete)
        {
            check(measured.has_value() && measured.value() == SecondaryMeasureProgress::complete, "measurement reaches its selected-frame boundary");
            break;
        }
    }

    check(std::abs(provider.reference_rms() - rms_pcm24(samples, 1U, 5000U)) < 1.0e-12, "measured excerpt RMS matches decoded PCM24 samples, including zeros");
    check(provider.measured_digest() == digest_pcm24(samples, 1U, 5000U), "excerpt digest hashes canonical little-endian PCM24 bytes");
    std::array<float, 2048U> output{};
    std::array<float, 1U> alignment_probe{};

    for (std::uint64_t first = 1U; first <= 4U; ++first)
    {
        check(provider.read(first, alignment_probe).is_ok(), "one-frame alignment probe read succeeds");
    }

    check(provider.read(1U, std::span{output}.first(2048U)).is_ok(), "first bounded source read succeeds");
    check(provider.read(2049U, std::span{output}.first(2048U)).is_ok(), "second bounded source read succeeds");

    for (std::size_t index = 0U; index < output.size(); ++index)
    {
        check(output[index] == audio::pcm24_to_float(samples[2049U + index]), "read returns exact decoded PCM24 sample");
    }

    check(provider.read(1U + 5000U, {}).is_ok(), "empty read at excerpt end is permitted");
    check(provider.read(1U + 5000U, std::span{output}.first(1U)).code == m110::StatusCode::invalid_argument, "nonempty read beyond excerpt is rejected");
    check(all_alignment_matches, "each FatFs destination alignment matches its file offset modulo four");
    check(alignment_offsets_mask == 0x0FU, "source offsets covering all four alignment classes are exercised");
    check(maximum_read <= secondary_wav_bytes_per_read && read_calls < 40U, "FatFs reads are bounded to at most 2048 PCM24 frames per request");
    check(provider.close().is_ok() && open_handles == 0U, "explicit close releases the only file handle");
}

void names_formats_and_excerpt_bounds()
{
    reset_fake();
    const auto samples = test_samples(16U);
    file_bytes = pcm24_wav(samples);
    Rt1170SecondaryWav provider;

    for (const std::string_view bad : {"", "../TEST.WAV", "..TEST.WAV", "A/B.WAV", "A\\B.WAV", "A:B.WAV", "bad*.WAV", "bad?.WAV", "bad<.WAV", "bad>.WAV", "bad|.WAV", "bad\".WAV", "TRAIL..WAV",
                                       "NOEXT.TXT", "NONASCII-\xC3\xA9.WAV"})
    {
        check(provider.open(bad, {}).code == m110::StatusCode::invalid_argument, "invalid basename is rejected");
    }

    const std::string max_name = "A" + std::string(250U, 'B') + ".WAV";
    expected_path = "2:/WG/" + max_name;
    check(max_name.size() == secondary_wav_basename_limit && provider.open(max_name, {}).is_ok(), "255-byte ASCII basename is accepted");
    check(provider.close().is_ok(), "max-length-name handle closes");
    const std::string too_long = "A" + std::string(251U, 'B') + ".WAV";
    check(provider.open(too_long, {}).code == m110::StatusCode::invalid_argument, "256-byte basename is refused");
    check(read_calls < 80U, "invalid names fail before FatFs reads");
    expected_path = "2:/WG/TEST.WAV";
    file_bytes = pcm24_wav(samples, 1U, 16U);
    check(provider.open("TEST.WAV", {}).code == m110::StatusCode::invalid_configuration, "PCM16 WAV is refused");
    file_bytes = pcm24_wav(samples, 2U, 24U);
    check(provider.open("TEST.WAV", {}).code == m110::StatusCode::invalid_configuration, "stereo PCM24 WAV is refused");
    file_bytes = pcm24_wav(samples);
    file_bytes.resize(8U);
    check(provider.open("TEST.WAV", {}).code == m110::StatusCode::invalid_configuration, "truncated RIFF header is refused");
    check(open_handles == 0U, "format refusals close their temporary file handle");
    file_bytes = pcm24_wav(samples);
    check(provider.open("TEST.WAV", excerpt_params(16U, 0U)).code == m110::StatusCode::invalid_argument, "source start at EOF is refused");
    check(provider.open("TEST.WAV", excerpt_params(12U, 5U)).code == m110::StatusCode::invalid_argument, "explicit excerpt extending past EOF is refused");
    check(provider.open("TEST.WAV", excerpt_params(12U, 0U)).is_ok() && provider.selected_frames() == 4U, "zero-duration excerpt selects exactly the remaining frames through EOF");
    std::array<float, 4U> tail{};
    const auto tail_measure = provider.measure_step(tail);
    check(tail_measure.has_value() && tail_measure.value() == SecondaryMeasureProgress::complete && std::abs(provider.reference_rms() - rms_pcm24(samples, 12U, 4U)) < 1.0e-12,
          "implicit EOF excerpt measures its complete tail");
    check(provider.close().is_ok() && open_handles == 0U, "implicit EOF excerpt closes without a leaked handle");
}

void measurement_and_read_faults()
{
    reset_fake();
    std::vector<audio::Pcm24Sample> silent(20U, 0);
    file_bytes = pcm24_wav(silent);
    Rt1170SecondaryWav provider;
    check(provider.open("TEST.WAV", {}).is_ok(), "silent source opens for measurement");
    std::array<float, 20U> scratch{};
    const auto silent_result = provider.measure_step(scratch);
    check(!silent_result.has_value() && silent_result.status().code == m110::StatusCode::invalid_configuration, "silent excerpt fails calibration");
    check(!provider.read(0U, scratch).is_ok(), "a silent failed calibration never enables reads");
    check(provider.close().is_ok(), "silent-source handle closes");
    reset_fake();
    file_bytes = pcm24_wav(test_samples(64U));
    check(provider.open("TEST.WAV", {}).is_ok(), "source opens before short-read fault injection");
    short_next_read = true;
    auto short_result = provider.measure_step(scratch);
    check(!short_result.has_value() && short_result.status().code == m110::StatusCode::io_error, "short FatFs payload read fails calibration");
    const auto after_short = provider.measure_step(scratch);
    check(!after_short.has_value() && std::strcmp(after_short.status().message, short_result.status().message) == 0, "measurement I/O fault remains latched");
    check(provider.close().is_ok(), "short-read source closes");
    reset_fake();
    file_bytes = pcm24_wav(test_samples(64U));
    check(provider.open("TEST.WAV", {}).is_ok(), "source opens before read fault injection");
    std::array<float, 64U> full_scratch{};
    auto measured = provider.measure_step(full_scratch);
    check(measured.has_value() && measured.value() == SecondaryMeasureProgress::complete, "small source calibration completes in one call");
    fail_next_read = true;
    std::array<float, 8U> output{};
    const auto first_read = provider.read(0U, output);
    const auto calls_after_failure = read_calls;
    const auto retry_read = provider.read(0U, output);
    check(first_read.code == m110::StatusCode::io_error && retry_read.message == first_read.message, "source read fault returns and remains latched");
    check(read_calls == calls_after_failure, "latched read fault prevents a retry against a failed file");
    check(provider.close().is_ok(), "read-fault source closes");
    reset_fake();
    file_bytes = pcm24_wav(test_samples(64U));
    check(provider.open("TEST.WAV", {}).is_ok(), "source opens before seek fault injection");
    fail_next_seek = true;
    const auto seek_result = provider.measure_step(full_scratch);
    check(!seek_result.has_value() && std::strcmp(seek_result.status().message, secondary_wav_seek_failed.message) == 0, "FatFs seek failure is returned distinctly");
    check(provider.close().is_ok(), "seek-fault source closes");
    reset_fake();
    file_bytes = pcm24_wav(test_samples(64U));
    check(provider.open("TEST.WAV", {}).is_ok(), "source opens before close fault injection");
    fail_close = true;
    const auto close_result = provider.close();
    const auto closes_after_failure = close_calls;
    const auto second_close = provider.close();
    check(close_result.code == m110::StatusCode::io_error && !second_close.is_ok(), "failed close remains a failure while the file is still owned");
    check(close_calls == closes_after_failure + 1U && open_handles == 1U, "failed close retains the handle for a later retry");
    fail_close = false;
    check(provider.close().is_ok() && open_handles == 0U, "cleanup retries the original handle after the failure clears");
    const auto recovered_closes = close_calls;
    check(provider.close().is_ok() && close_calls == recovered_closes, "successful cleanup makes subsequent close idempotent");
}

} // namespace

extern "C"
{
    FRESULT f_open(FIL* file, const char* path, unsigned char mode)
    {
        if (fail_open || std::string_view{path} != expected_path || mode != FA_READ)
        {
            return FR_DISK_ERR;
        }

        file->data = file_bytes.data();
        file->size = static_cast<FSIZE_t>(file_bytes.size());
        file->position = 0U;
        file->opened = 1;
        ++open_handles;
        return FR_OK;
    }

    FRESULT f_read(FIL* file, void* destination, UINT requested, UINT* actual)
    {
        ++read_calls;
        maximum_read = std::max(maximum_read, requested);
        all_alignment_matches = all_alignment_matches && (reinterpret_cast<std::uintptr_t>(destination) % 4U) == (file->position % 4U);
        alignment_offsets_mask = static_cast<std::uint8_t>(alignment_offsets_mask | (1U << (file->position % 4U)));
        *actual = 0U;

        if (!file->opened || fail_next_read)
        {
            fail_next_read = false;
            return FR_DISK_ERR;
        }

        const UINT available = file->size - file->position;
        const UINT count = std::min(requested, available);
        *actual = short_next_read && count != 0U ? count - 1U : count;
        short_next_read = false;
        std::memcpy(destination, file_bytes.data() + file->position, *actual);
        file->position += *actual;
        return FR_OK;
    }

    FRESULT f_lseek(FIL* file, FSIZE_t offset)
    {
        if (!file->opened || offset > file->size || fail_next_seek)
        {
            fail_next_seek = false;
            return FR_DISK_ERR;
        }

        file->position = offset;
        return FR_OK;
    }

    FRESULT f_close(FIL* file)
    {
        ++close_calls;

        if (!file->opened)
        {
            return FR_DISK_ERR;
        }

        if (fail_close)
        {
            return FR_DISK_ERR;
        }

        file->opened = 0;
        --open_handles;
        return FR_OK;
    }
}

int main()
{
    pcm24_measurement_and_reads();
    names_formats_and_excerpt_bounds();
    measurement_and_read_faults();

    if (failures != 0)
    {
        std::fprintf(stderr, "%d secondary WAV host check(s) failed\n", failures);
        return 1;
    }

    std::puts("secondary WAV provider host checks passed");
    return 0;
}
