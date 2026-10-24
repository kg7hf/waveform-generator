// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// Host driver of the Watterson validation battery: renders a mono 48 kHz WAV
// through the signal-lab engine with a WattersonChannel, block by block, so a
// multi-hour row needs a few kilobytes of sample memory.
// Guide: docs/design/watterson-validation.md; the stage: docs/modules/signal-lab-watterson.md.
//
//   wfg_watterson_render --scenario case.json (--source in.wav | --silence-seconds S)
//                        --output out.wav [--sidecar out.json] [--truth-out taps.bin]
//                        [--frequency-truth-out frequency.csv]
//                        [--block N] [--seed N] [--reference-rms R]
//
// Why a driver of its own: signal_lab_render (host/render) does not pass a
// WattersonChannel yet (design WP7, in the playlist workflow's paths). This one
// runs the same Engine, the same contiguous blocks and the same PCM24 quantiser
// (det::quantize_pcm24), so the digest it prints is the engine's output digest.
//
// --truth-out writes the channel truth (survey gap C10): every TapRecord the stage
// reports, as "WFGTAPS1" and then 20-byte little-endian records (u64 node frame,
// u32 path, f32 real, f32 imaginary). With the sidecar's per-path report (M, a_i,
// phase steps, delays) the 48 kHz path gains are rebuilt exactly (design 9.7).
// --frequency-truth-out is separate: a bounded CSV sampled every 64 engine input
// frames, when a global frequency_shift stage is present. WFGTAPS1 stays byte-for-byte
// unchanged; the CSV's format is in docs/design/frequency-truth-export.md.
//
// An offline host tool (docs/coding-standard.md, "Accepted boundaries"): it
// allocates its file buffers and paths, and it never runs on the board.

#include "common/host_critical_error.hpp"
#include "common/pcm24.hpp"
#include "signal_lab/awgn.hpp"
#include "signal_lab/convert.hpp"
#include "signal_lab/det_math.hpp"
#include "signal_lab/frequency_shift.hpp"
#include "signal_lab/impairment.hpp"
#include "signal_lab/mixer.hpp"
#include "signal_lab/scenario.hpp"
#include "signal_lab/units/angle.hpp"
#include "signal_lab/units/decibel.hpp"
#include "signal_lab/units/frequency.hpp"
#include "signal_lab/watterson.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace
{

namespace sl = signal_lab;
namespace units = signal_lab::units;
namespace audio = waveform_generator::audio;

// ---- constants ----

// The tool's identity in the sidecar; raise the version with any change of output.
constexpr std::string_view tool_name = "wfg_watterson_render";
constexpr std::string_view tool_version = "1";
constexpr std::string_view sidecar_schema = "signal-lab.watterson-render/1";
constexpr std::string_view truth_magic = "WFGTAPS1";

// Exit status: 0 done, 1 a file failed, 2 a usage, scenario or engine error,
// or a critical error (common/host_critical_error.hpp).
constexpr int exit_ok = 0;
constexpr int exit_file = 1;
constexpr int exit_usage = 2;

// RIFF/WAVE layout (Microsoft RIFF; WAVEFORMATEX and WAVEFORMATEXTENSIBLE). Offsets are in bytes.
constexpr std::size_t riff_header_bytes = 12U; // "RIFF", size, "WAVE"
constexpr std::size_t riff_form_at = 8U;       // "WAVE"
constexpr std::size_t chunk_header_bytes = 8U; // id, size
constexpr std::size_t chunk_size_at = 4U;
constexpr std::size_t fmt_basic_bytes = 16U;      // WAVEFORMAT + wBitsPerSample
constexpr std::size_t fmt_extensible_bytes = 40U; // WAVEFORMATEXTENSIBLE
constexpr std::size_t fmt_channels_at = 2U;
constexpr std::size_t fmt_rate_at = 4U;
constexpr std::size_t fmt_bits_at = 14U;
constexpr std::size_t fmt_subformat_at = 24U; // the first two bytes of the SubFormat GUID are the format tag
// The 44-byte header this tool writes: RIFF, fmt (16 bytes), data.
constexpr std::size_t wav_header_bytes = 44U;
constexpr std::size_t header_riff_size_at = 4U;
constexpr std::size_t header_wave_fmt_at = 8U; // "WAVEfmt "
constexpr std::size_t header_fmt_size_at = 16U;
constexpr std::size_t header_format_at = 20U;
constexpr std::size_t header_channels_at = 22U;
constexpr std::size_t header_rate_at = 24U;
constexpr std::size_t header_byte_rate_at = 28U;
constexpr std::size_t header_block_align_at = 32U;
constexpr std::size_t header_bits_at = 34U;
constexpr std::size_t header_data_tag_at = 36U;
constexpr std::size_t header_data_size_at = 40U;
constexpr std::uint16_t format_pcm = 1U;
constexpr std::uint16_t format_float = 3U;
constexpr std::uint16_t format_extensible = 0xFFFEU;
constexpr std::uint16_t mono = 1U;
constexpr std::uint16_t bits_pcm16 = 16U;
constexpr std::uint16_t bits_pcm24 = 24U;
constexpr std::uint16_t bits_float32 = 32U;
constexpr std::uint32_t bits_per_byte = 8U;
constexpr std::uint64_t riff_size_limit = 0xFFFFFFFFU;
constexpr std::uint64_t byte_mask = 0xFFU;

// PCM16 full scale, as signal_lab_render reads a legacy PCM16 source.
constexpr double pcm16_scale = 32768.0;
constexpr std::size_t pcm24_width = audio::pcm24_bytes_per_sample;

// Truth records: u64 frame, u32 path, f32 real, f32 imaginary; flushed in batches.
constexpr std::size_t truth_frame_at = 0U;
constexpr std::size_t truth_path_at = 8U;
constexpr std::size_t truth_real_at = 12U;
constexpr std::size_t truth_imag_at = 16U;
constexpr std::size_t truth_record_bytes = 20U;
constexpr std::size_t truth_batch_records = 65536U;

// Frames read per pass of the reference pre-pass.
constexpr std::size_t prepass_frames = 65536U;
// Characters of a shortest round-trip double, and of a 64-bit hexadecimal digest.
constexpr std::size_t double_characters = 32U;
constexpr std::size_t hex_digits = 16U;
constexpr int hex_base = 16;

// ---- the non-owned boundaries: the RIFF format, iostreams, C stdio, main ----
// Each conversion they need is written once here; the rest of the file has no cast.

// A little-endian RIFF field assembled from `width` bytes into 64 bits, at its own
// width: exact, because no more bits than the field's were assembled.
template <typename Field> [[nodiscard]] constexpr Field field_of(std::uint64_t value) noexcept
{
    return static_cast<Field>(value);
}

// A buffer size as the iostream count type: exact, every buffer here is far below 2^63.
[[nodiscard]] std::streamsize stream_size(std::size_t bytes) noexcept
{
    return static_cast<std::streamsize>(bytes);
}

// main()'s argument count, which the C++ entry point gives as an int (never negative).
[[nodiscard]] std::size_t argument_count(int argc) noexcept
{
    return argc > 0 ? static_cast<std::size_t>(argc) : 0U;
}

// One diagnostic line on a C stream; a failed write of a diagnostic has nowhere to go.
void put_line(std::FILE* stream, std::string_view text) noexcept
{
    [[maybe_unused]] const bool written = std::fwrite(text.data(), 1U, text.size(), stream) == text.size() && std::fputc('\n', stream) == '\n';
}

// ---- bytes ----

[[nodiscard]] std::uint8_t byte_at(std::span<const char> bytes, std::size_t at) noexcept
{
    return std::bit_cast<std::uint8_t>(bytes[at]);
}

[[nodiscard]] std::uint64_t read_le(std::span<const char> bytes, std::size_t at, std::size_t width) noexcept
{
    std::uint64_t value = 0U;

    for (std::size_t index = 0U; index < width; ++index)
    {
        value |= std::uint64_t{byte_at(bytes, at + index)} << (bits_per_byte * index);
    }

    return value;
}

[[nodiscard]] std::uint16_t read_u16(std::span<const char> bytes, std::size_t at) noexcept
{
    return field_of<std::uint16_t>(read_le(bytes, at, sizeof(std::uint16_t)));
}

[[nodiscard]] std::uint32_t read_u32(std::span<const char> bytes, std::size_t at) noexcept
{
    return field_of<std::uint32_t>(read_le(bytes, at, sizeof(std::uint32_t)));
}

void put_le(std::span<char> bytes, std::size_t at, std::uint64_t value, std::size_t width) noexcept
{
    for (std::size_t index = 0U; index < width; ++index)
    {
        bytes[at + index] = std::bit_cast<char>(field_of<std::uint8_t>((value >> (bits_per_byte * index)) & byte_mask));
    }
}

[[nodiscard]] bool tag_is(std::span<const char> bytes, std::size_t at, std::string_view tag) noexcept
{
    return std::ranges::equal(bytes.subspan(at, tag.size()), tag);
}

void put_tag(std::span<char> bytes, std::size_t at, std::string_view tag) noexcept
{
    std::ranges::copy(tag, bytes.subspan(at, tag.size()).begin());
}

void put_error(std::string_view text) noexcept
{
    put_line(stderr, text);
}

// A file this driver cannot read: io_error with the static message.
[[nodiscard]] constexpr m110::Status unreadable(const char* message) noexcept
{
    return m110::Status{.code = m110::StatusCode::io_error, .message = message};
}

// A file this driver cannot write: io_error with the static message.
[[nodiscard]] constexpr m110::Status unwritable(const char* message) noexcept
{
    return m110::Status{.code = m110::StatusCode::io_error, .message = message};
}

// A stream that gave fewer bytes than asked for, or none.
constexpr m110::Status short_read = unreadable("a short or failed read");

// A stream that did not take every byte.
constexpr m110::Status failed_write = unwritable("a failed write");

[[nodiscard]] m110::Status read_exact(std::istream& stream, std::span<char> bytes)
{
    return bytes.empty() || !stream.read(bytes.data(), stream_size(bytes.size())).fail() ? m110::Status::success() : short_read;
}

[[nodiscard]] m110::Status write_exact(std::ostream& stream, std::span<const char> bytes)
{
    return bytes.empty() || !stream.write(bytes.data(), stream_size(bytes.size())).fail() ? m110::Status::success() : failed_write;
}

// An input this driver refuses: invalid_argument with the static message.
[[nodiscard]] constexpr m110::Status refused(const char* message) noexcept
{
    return m110::Status{.code = m110::StatusCode::invalid_argument, .message = message};
}

void put_out(std::string_view text) noexcept
{
    put_line(stdout, text);
}

// ---- the source: a mono 48 kHz WAV (PCM16, PCM24 or float32), or digital silence ----

// What a failed read of the source's data gives.
constexpr m110::Status source_unreadable = unreadable("cannot read the source");

enum class SampleFormat : std::uint8_t
{
    pcm16,
    pcm24,
    float32,
    silence,
};

class SampleSource
{
public:
    // Opens a WAV and stops at the start of its data chunk: success, or why not.
    [[nodiscard]] m110::Status open(const std::string& path);

    void open_silence(std::uint64_t frames) noexcept
    {
        mFormat = SampleFormat::silence;
        mFrames = frames;
        mRemaining = frames;
    }

    [[nodiscard]] std::uint64_t frames() const noexcept { return mFrames; }

    // Reads up to output.size() frames as normalised floats: the count read,
    // 0 at the end of the data; or source_unreadable for a read that fails
    // before the end the data chunk declares.
    [[nodiscard]] m110::Result<std::size_t> read(std::span<float> output);

private:
    [[nodiscard]] m110::Status read_format(std::span<const char> body);
    [[nodiscard]] float decode(std::span<const char> sample) const noexcept;

    std::ifstream mFile{};
    std::vector<char> mBytes{};
    std::uint64_t mFrames{};
    std::uint64_t mRemaining{};
    std::size_t mBytesPerFrame{};
    SampleFormat mFormat{SampleFormat::silence};
};

m110::Status SampleSource::read_format(std::span<const char> body)
{
    if (body.size() < fmt_basic_bytes)
    {
        return refused("fmt chunk too short");
    }

    std::uint16_t tag = read_u16(body, 0U);
    const std::uint16_t channels = read_u16(body, fmt_channels_at);
    const std::uint32_t rate = read_u32(body, fmt_rate_at);
    const std::uint16_t bits = read_u16(body, fmt_bits_at);

    if (tag == format_extensible && body.size() >= fmt_extensible_bytes)
    {
        tag = read_u16(body, fmt_subformat_at);
    }

    if (channels != mono || rate != sl::sample_rate_hz)
    {
        return refused("source must be mono at 48000 Hz");
    }

    if (tag == format_pcm && bits == bits_pcm16)
    {
        mFormat = SampleFormat::pcm16;
    }
    else if (tag == format_pcm && bits == bits_pcm24)
    {
        mFormat = SampleFormat::pcm24;
    }
    else if (tag == format_float && bits == bits_float32)
    {
        mFormat = SampleFormat::float32;
    }
    else
    {
        return refused("source must be PCM16, PCM24 or float32");
    }

    mBytesPerFrame = bits / bits_per_byte;
    return m110::Status::success();
}

m110::Status SampleSource::open(const std::string& path)
{
    mFile.open(path, std::ios::binary);
    std::array<char, riff_header_bytes> riff{};

    if (!mFile || !read_exact(mFile, riff).is_ok() || !tag_is(riff, 0U, "RIFF") || !tag_is(riff, riff_form_at, "WAVE"))
    {
        return unreadable("cannot read a RIFF/WAVE source");
    }

    bool have_format = false;

    for (;;)
    {
        std::array<char, chunk_header_bytes> header{};

        if (!read_exact(mFile, header).is_ok())
        {
            return refused("source has no data chunk");
        }

        const std::uint32_t size = read_u32(header, chunk_size_at);

        if (tag_is(header, 0U, "data"))
        {
            if (!have_format)
            {
                return refused("data chunk before the fmt chunk");
            }

            mFrames = size / mBytesPerFrame;
            mRemaining = mFrames;
            return m110::Status::success();
        }

        // Every other chunk (fmt, LIST, ...) is small: read it whole, with its pad byte.
        std::vector<char> body(size + (size & 1U));

        if (!read_exact(mFile, body).is_ok())
        {
            return unreadable("truncated chunk");
        }

        if (tag_is(header, 0U, "fmt "))
        {
            if (const m110::Status format = read_format(body); !format.is_ok())
            {
                return format;
            }

            have_format = true;
        }
    }
}

float SampleSource::decode(std::span<const char> sample) const noexcept
{
    switch (mFormat)
    {
    case SampleFormat::pcm16:
        return sl::to_float(sl::as_double(std::bit_cast<std::int16_t>(read_u16(sample, 0U))) / pcm16_scale);

    case SampleFormat::pcm24:
    {
        const std::array<std::uint8_t, pcm24_width> packed{byte_at(sample, 0U), byte_at(sample, 1U), byte_at(sample, 2U)};
        return audio::pcm24_to_float(audio::read_pcm24_le(packed.data()));
    }

    case SampleFormat::float32:
        return std::bit_cast<float>(read_u32(sample, 0U));

    case SampleFormat::silence:
        break;
    }

    return 0.0F;
}

m110::Result<std::size_t> SampleSource::read(std::span<float> output)
{
    const std::size_t count = sl::to_size(std::min<std::uint64_t>(output.size(), mRemaining));

    if (mFormat == SampleFormat::silence)
    {
        std::ranges::fill(output.first(count), 0.0F);
        mRemaining -= count;
        return count;
    }

    mBytes.resize(count * mBytesPerFrame);

    if (!read_exact(mFile, mBytes).is_ok())
    {
        return source_unreadable;
    }

    const std::span<const char> bytes{mBytes};

    for (std::size_t frame = 0U; frame < count; ++frame)
    {
        output[frame] = decode(bytes.subspan(frame * mBytesPerFrame, mBytesPerFrame));
    }

    mRemaining -= count;
    return count;
}

// ---- the output: PCM24 mono 48 kHz, header patched at close ----

// A WAV whose sizes do not fit the RIFF header's 32 bits.
constexpr m110::Status riff_too_large = unwritable("the WAV exceeds the RIFF size limit");

class Pcm24Writer
{
public:
    // Creates the file with the header of an empty WAV: success, or why not.
    [[nodiscard]] m110::Status open(const std::string& path)
    {
        mFile.open(path, std::ios::binary | std::ios::trunc);
        return mFile ? write_header() : unwritable("cannot create the file");
    }

    // Appends the samples as PCM24: success, or the first failed write, which
    // every later write and close() report too.
    [[nodiscard]] m110::Status write(std::span<const float> samples);

    // Patches the RIFF and data sizes and closes the file: success, or the
    // first failed write, riff_too_large, or a failed seek or close.
    [[nodiscard]] m110::Status close();

private:
    [[nodiscard]] m110::Status write_header();

    std::ofstream mFile{};
    std::vector<char> mBytes{};
    std::uint64_t mFrames{};
    m110::Status mWritten{};
};

m110::Status Pcm24Writer::write_header()
{
    const std::uint64_t data_bytes = mFrames * pcm24_width;
    const std::uint64_t riff_bytes = data_bytes + wav_header_bytes - chunk_header_bytes;

    if (riff_bytes > riff_size_limit)
    {
        return riff_too_large;
    }

    std::array<char, wav_header_bytes> header{};
    put_tag(header, 0U, "RIFF");
    put_le(header, header_riff_size_at, riff_bytes, sizeof(std::uint32_t));
    put_tag(header, header_wave_fmt_at, "WAVEfmt ");
    put_le(header, header_fmt_size_at, fmt_basic_bytes, sizeof(std::uint32_t));
    put_le(header, header_format_at, format_pcm, sizeof(std::uint16_t));
    put_le(header, header_channels_at, mono, sizeof(std::uint16_t));
    put_le(header, header_rate_at, sl::sample_rate_hz, sizeof(std::uint32_t));
    put_le(header, header_byte_rate_at, std::uint64_t{sl::sample_rate_hz} * pcm24_width, sizeof(std::uint32_t));
    put_le(header, header_block_align_at, pcm24_width, sizeof(std::uint16_t));
    put_le(header, header_bits_at, bits_pcm24, sizeof(std::uint16_t));
    put_tag(header, header_data_tag_at, "data");
    put_le(header, header_data_size_at, data_bytes, sizeof(std::uint32_t));
    return write_exact(mFile, header);
}

m110::Status Pcm24Writer::write(std::span<const float> samples)
{
    mBytes.resize(samples.size() * pcm24_width);
    const std::span<char> bytes{mBytes};

    for (std::size_t index = 0U; index < samples.size(); ++index)
    {
        std::array<std::uint8_t, pcm24_width> packed{};
        audio::write_pcm24_le(packed.data(), sl::det::quantize_pcm24(samples[index]));
        std::ranges::transform(packed, bytes.subspan(index * pcm24_width, pcm24_width).begin(), [](std::uint8_t byte) { return std::bit_cast<char>(byte); });
    }

    mFrames += samples.size();

    if (mWritten.is_ok())
    {
        mWritten = write_exact(mFile, mBytes);
    }

    return mWritten;
}

m110::Status Pcm24Writer::close()
{
    mFile.seekp(0);
    m110::Status patched = mWritten;

    if (patched.is_ok())
    {
        patched = mFile.fail() ? failed_write : write_header();
    }

    mFile.close();
    return patched.is_ok() && mFile.fail() ? failed_write : patched;
}

// ---- channel truth ----

class TruthWriter final : public sl::TapObserver
{
public:
    TruthWriter() = default;
    ~TruthWriter() = default;
    TruthWriter(const TruthWriter&) = delete;
    TruthWriter& operator=(const TruthWriter&) = delete;
    TruthWriter(TruthWriter&&) = delete;
    TruthWriter& operator=(TruthWriter&&) = delete;

    // Creates the file; the magic is written with the first records: success, or why not.
    [[nodiscard]] m110::Status open(const std::string& path)
    {
        mFile.open(path, std::ios::binary | std::ios::trunc);
        mBytes.resize(truth_magic.size());
        put_tag(mBytes, 0U, truth_magic);
        return mFile.is_open() && !mFile.fail() ? m110::Status::success() : unwritable("cannot create the file");
    }

    // The stage calls this from configure() and process(); it allocates (a host
    // diagnostic, design 9.7: the board passes no observer).
    void on_tap(const sl::TapRecord& record) noexcept override
    {
        const std::size_t at = mBytes.size();
        mBytes.resize(at + truth_record_bytes);
        const std::span<char> bytes{mBytes};
        put_le(bytes, at + truth_frame_at, record.node_frame.value(), sizeof(std::uint64_t));
        put_le(bytes, at + truth_path_at, record.path, sizeof(std::uint32_t));
        put_le(bytes, at + truth_real_at, std::bit_cast<std::uint32_t>(record.value.real), sizeof(std::uint32_t));
        put_le(bytes, at + truth_imag_at, std::bit_cast<std::uint32_t>(record.value.imag), sizeof(std::uint32_t));
        ++mRecords;

        if (mBytes.size() >= truth_batch_records * truth_record_bytes)
        {
            flush();
        }
    }

    // Writes the records not yet written and closes the file: success, or the
    // first failed write, or a failed close.
    [[nodiscard]] m110::Status close()
    {
        flush();
        mFile.close();
        return mWritten.is_ok() && mFile.fail() ? failed_write : mWritten;
    }

    [[nodiscard]] std::uint64_t records() const noexcept { return mRecords; }

private:
    void flush() noexcept
    {
        if (mWritten.is_ok())
        {
            mWritten = write_exact(mFile, mBytes);
        }

        mBytes.clear();
    }

    std::ofstream mFile{};
    std::vector<char> mBytes{};
    std::uint64_t mRecords{};
    m110::Status mWritten{};
};

// The frequency truth is intentionally independent of WFGTAPS1. Its fixed
// stride keeps a long validation run bounded in memory and output size.
constexpr std::string_view frequency_truth_magic = "WFGFREQ1";
constexpr std::uint64_t frequency_truth_stride_frames = 64U;
constexpr m110::Status frequency_truth_order = refused("frequency truth input frames are not contiguous");
constexpr m110::Status frequency_truth_range = refused("frequency truth frame range overflows uint64");
constexpr m110::Status frequency_truth_format = unwritable("cannot format a frequency truth record");

class FrequencyTruthWriter
{
public:
    FrequencyTruthWriter() = default;
    ~FrequencyTruthWriter() = default;
    FrequencyTruthWriter(const FrequencyTruthWriter&) = delete;
    FrequencyTruthWriter& operator=(const FrequencyTruthWriter&) = delete;
    FrequencyTruthWriter(FrequencyTruthWriter&&) = delete;
    FrequencyTruthWriter& operator=(FrequencyTruthWriter&&) = delete;

    // Opens a CSV with explicit frame and filter-delay coordinates.
    [[nodiscard]] m110::Status open(const std::string& path, const sl::FrequencyShiftParams& params, bool has_watterson, std::uint64_t input_frames)
    {
        mFile.open(path, std::ios::out | std::ios::binary | std::ios::trunc);

        if (!mFile)
        {
            return unwritable("cannot create the file");
        }

        const std::uint32_t watterson_delay = has_watterson ? sl::hilbert_delay : 0U;
        const std::uint32_t total_delay = sl::hilbert_delay + watterson_delay;
        const std::string metadata = std::string{frequency_truth_magic} + "\n# sample_rate_hz=" + std::to_string(sl::stream_rate.hertz()) +
                                     "\n# stride_frames=" + std::to_string(frequency_truth_stride_frames) + "\n# engine_input_frames=" + std::to_string(input_frames) +
                                     "\n# mode=" + (params.mode == sl::FrequencyShiftMode::offset ? "offset" : "sweep") +
                                     "\n# frame_coordinate=absolute_engine_input_frame_at_frequency_shift\n# "
                                     "frequency_shift_hilbert_delay_frames=" +
                                     std::to_string(sl::hilbert_delay) + "\n# watterson_hilbert_delay_frames=" + std::to_string(watterson_delay) +
                                     "\n# total_hilbert_delay_frames=" + std::to_string(total_delay) + "\nengine_input_frame,instantaneous_offset_hz\n";
        mWritten = write_exact(mFile, metadata);

        if (!mWritten.is_ok())
        {
            mFile.close();
        }

        return mWritten;
    }

    // Writes the frequency value at every stride frame in this contiguous input block.
    [[nodiscard]] m110::Status write(const sl::FrequencyShiftParams& params, std::uint64_t first_frame, std::size_t frames)
    {
        if (!mWritten.is_ok())
        {
            return mWritten;
        }

        if (first_frame != mNextInputFrame)
        {
            return frequency_truth_order;
        }

        if (frames > std::numeric_limits<std::uint64_t>::max() - first_frame)
        {
            return frequency_truth_range;
        }

        const std::uint64_t end_frame = first_frame + frames;

        while (mNextSampleFrame < end_frame)
        {
            if (const m110::Status written = write_record(params, mNextSampleFrame); !written.is_ok())
            {
                mWritten = written;
                return mWritten;
            }

            if (mNextSampleFrame > std::numeric_limits<std::uint64_t>::max() - frequency_truth_stride_frames)
            {
                mNextSampleFrame = std::numeric_limits<std::uint64_t>::max();
                break;
            }

            mNextSampleFrame += frequency_truth_stride_frames;
        }

        mNextInputFrame = end_frame;
        return m110::Status::success();
    }

    // Closes the CSV, preserving the first write or close failure.
    [[nodiscard]] m110::Status close()
    {
        mFile.close();
        return mWritten.is_ok() && mFile.fail() ? failed_write : mWritten;
    }

private:
    [[nodiscard]] m110::Status write_record(const sl::FrequencyShiftParams& params, std::uint64_t frame)
    {
        std::array<char, 96U> line{};
        char* cursor = line.data();
        const auto frame_text = std::to_chars(cursor, line.data() + line.size(), frame);

        if (frame_text.ec != std::errc{})
        {
            return frequency_truth_format;
        }

        cursor = frame_text.ptr;
        *cursor = ',';
        ++cursor;
        const double frequency = sl::instantaneous_frequency(params, units::SampleIndex{frame}, sl::stream_rate).in(units::Hz);
        const auto frequency_text = std::to_chars(cursor, line.data() + line.size(), frequency, std::chars_format::general, std::numeric_limits<double>::max_digits10);

        if (frequency_text.ec != std::errc{} || frequency_text.ptr == line.data() + line.size())
        {
            return frequency_truth_format;
        }

        cursor = frequency_text.ptr;
        *cursor = '\n';
        ++cursor;
        const m110::Status written = write_exact(mFile, std::span<const char>{line.data(), static_cast<std::size_t>(cursor - line.data())});
        return written;
    }

    std::ofstream mFile{};
    std::uint64_t mNextInputFrame{};
    std::uint64_t mNextSampleFrame{};
    m110::Status mWritten{};
};

// ---- options ----

struct Options
{
    std::string scenario;
    std::string source;
    std::string output;
    std::string sidecar;
    std::string truth;
    std::string frequency_truth;
    std::optional<double> silence_seconds{};
    std::optional<double> reference_rms{};
    std::optional<std::uint64_t> seed{};
    std::size_t block{sl::engine_block_frames};
};

[[nodiscard]] std::optional<double> parse_double(std::string_view text) noexcept
{
    double value{};
    const char* const first = std::to_address(text.begin());
    const char* const last = std::to_address(text.end());
    const auto parsed = std::from_chars(first, last, value);
    return parsed.ec == std::errc{} && parsed.ptr == last && std::isfinite(value) ? std::optional{value} : std::nullopt;
}

[[nodiscard]] std::optional<std::uint64_t> parse_unsigned(std::string_view text) noexcept
{
    std::uint64_t value{};
    const char* const first = std::to_address(text.begin());
    const char* const last = std::to_address(text.end());
    const auto parsed = std::from_chars(first, last, value);
    return parsed.ec == std::errc{} && parsed.ptr == last ? std::optional{value} : std::nullopt;
}

[[nodiscard]] std::string* text_option(std::string_view flag, Options& options) noexcept
{
    if (flag == "--scenario")
    {
        return &options.scenario;
    }

    if (flag == "--source")
    {
        return &options.source;
    }

    if (flag == "--output")
    {
        return &options.output;
    }

    if (flag == "--sidecar")
    {
        return &options.sidecar;
    }

    if (flag == "--truth-out")
    {
        return &options.truth;
    }

    return flag == "--frequency-truth-out" ? &options.frequency_truth : nullptr;
}

// A flag that is no option, or an option's value that does not parse.
constexpr m110::Status bad_option = refused("unknown flag or bad value");

// A canonical, absolute spelling for collision checks, including paths whose
// final file does not exist yet. `weakly_canonical` resolves existing parent
// directories but leaves a not-yet-created file name intact.
[[nodiscard]] m110::Result<std::string> normalized_path(std::string_view value)
{
    std::error_code error;
    const std::filesystem::path absolute = std::filesystem::absolute(std::filesystem::path{std::string{value}}, error);

    if (error)
    {
        return refused("cannot resolve an artifact path");
    }

    std::filesystem::path canonical = std::filesystem::weakly_canonical(absolute, error);

    if (error)
    {
        return refused("cannot resolve an artifact path");
    }

    std::string key = canonical.generic_string();
#if defined(_WIN32)

    for (char& letter : key)
    {
        if (letter >= 'A' && letter <= 'Z')
        {
            letter = static_cast<char>(letter - 'A' + 'a');
        }
    }

#endif
    return key;
}

[[nodiscard]] m110::Result<bool> paths_equivalent(std::string_view first, std::string_view second, std::string_view first_key, std::string_view second_key)
{
    if (first_key == second_key)
    {
        return true;
    }

    const std::filesystem::path first_path{std::string{first}};
    const std::filesystem::path second_path{std::string{second}};
    std::error_code error;
    const bool equivalent = std::filesystem::equivalent(first_path, second_path, error);

    if (!error)
    {
        return equivalent;
    }

    // `equivalent` reports a missing operand as an error. Treat that as
    // non-equivalence only when the filesystem confirms ordinary absence.
    // A dangling symlink can still redirect a future writer, so fail closed.
    if (error != std::errc::no_such_file_or_directory)
    {
        return refused("cannot verify artifact path equivalence");
    }

    const std::filesystem::file_status first_status = std::filesystem::symlink_status(first_path, error);
    const bool first_missing = first_status.type() == std::filesystem::file_type::not_found && (!error || error == std::errc::no_such_file_or_directory);

    if (error && !first_missing)
    {
        return refused("cannot inspect artifact path");
    }

    error.clear();
    const std::filesystem::file_status second_status = std::filesystem::symlink_status(second_path, error);
    const bool second_missing = second_status.type() == std::filesystem::file_type::not_found && (!error || error == std::errc::no_such_file_or_directory);

    if (error && !second_missing)
    {
        return refused("cannot inspect artifact path");
    }

    if (first_status.type() == std::filesystem::file_type::symlink || second_status.type() == std::filesystem::file_type::symlink)
    {
        return refused("cannot verify artifact path equivalence");
    }

    if (!first_missing && !second_missing)
    {
        return refused("cannot verify artifact path equivalence");
    }

    return false;
}

// Writers must not overwrite inputs or one another; in particular, keep a
// separate frequency CSV from the established binary WFGTAPS1 artifact.
[[nodiscard]] m110::Status validate_distinct_paths(const Options& options)
{
    const std::array<std::string_view, 6U> paths{options.scenario, options.source, options.output, options.sidecar, options.truth, options.frequency_truth};
    std::array<std::string, 6U> keys{};

    for (std::size_t index = 0U; index < paths.size(); ++index)
    {
        if (paths[index].empty())
        {
            continue;
        }

        const auto normalized = normalized_path(paths[index]);

        if (!normalized.has_value())
        {
            return normalized.status();
        }

        keys[index] = normalized.value();
    }

    constexpr std::size_t first_writer = 2U;

    for (std::size_t writer = first_writer; writer < paths.size(); ++writer)
    {
        if (paths[writer].empty())
        {
            continue;
        }

        for (std::size_t input = 0U; input < first_writer; ++input)
        {
            if (paths[input].empty())
            {
                continue;
            }

            const auto equivalent = paths_equivalent(paths[writer], paths[input], keys[writer], keys[input]);

            if (!equivalent.has_value())
            {
                return equivalent.status();
            }

            if (equivalent.value())
            {
                return refused("artifact paths must not overwrite inputs or one another");
            }
        }

        for (std::size_t other_writer = first_writer; other_writer < writer; ++other_writer)
        {
            if (paths[other_writer].empty())
            {
                continue;
            }

            const auto equivalent = paths_equivalent(paths[writer], paths[other_writer], keys[writer], keys[other_writer]);

            if (!equivalent.has_value())
            {
                return equivalent.status();
            }

            if (equivalent.value())
            {
                return refused("artifact paths must not overwrite inputs or one another");
            }
        }
    }

    return m110::Status::success();
}

[[nodiscard]] bool scenario_has_stage(const sl::Scenario& scenario, sl::StageType type) noexcept
{
    const auto stages = std::span{scenario.stages}.first(scenario.stage_count);
    return std::ranges::any_of(stages, [type](const sl::Stage& stage) { return stage.type == type; });
}

// A numeric flag: success, or bad_option when the flag is not numeric or its
// value does not parse.
[[nodiscard]] m110::Status number_option(std::string_view flag, std::string_view value, Options& options) noexcept
{
    if (flag == "--silence-seconds" || flag == "--reference-rms")
    {
        const std::optional<double> number = parse_double(value);
        const bool positive = number.has_value() && *number > 0.0;
        (flag == "--silence-seconds" ? options.silence_seconds : options.reference_rms) = positive ? number : std::nullopt;
        return positive ? m110::Status::success() : bad_option;
    }

    const std::optional<std::uint64_t> number = parse_unsigned(value);

    if (flag == "--seed")
    {
        options.seed = number;
        return number.has_value() ? m110::Status::success() : bad_option;
    }

    if (flag == "--block" && number.has_value() && *number >= 1U && *number <= sl::engine_block_frames)
    {
        options.block = sl::to_size(*number);
        return m110::Status::success();
    }

    return bad_option;
}

constexpr std::string_view usage = "usage: wfg_watterson_render --scenario case.json (--source in.wav | --silence-seconds S) --output out.wav"
                                   " [--sidecar out.json] [--truth-out taps.bin] [--frequency-truth-out frequency.csv]"
                                   " [--block 1..2048] [--seed N] [--reference-rms R]";

// The options the command line gives, or the refusal of it (its reason, with
// the flag it names, already on stderr).
[[nodiscard]] m110::Result<Options> parse_arguments(std::span<char*> arguments)
{
    Options options;

    for (std::size_t index = 1U; index < arguments.size(); index += 2U)
    {
        const std::string_view flag{arguments[index]};

        if (index + 1U >= arguments.size())
        {
            put_error(std::string{"missing value for "} + std::string{flag});
            return refused("an option without its value");
        }

        const std::string_view value{arguments[index + 1U]};
        std::string* const text = text_option(flag, options);

        if (text != nullptr)
        {
            *text = value;
        }
        else if (const m110::Status number = number_option(flag, value, options); !number.is_ok())
        {
            put_error(std::string{number.message} + ": " + std::string{flag});
            return number;
        }
    }

    const bool one_source = options.source.empty() != !options.silence_seconds.has_value();

    if (options.scenario.empty() || options.output.empty() || !one_source)
    {
        put_error(usage);
        return refused("a scenario, an output and one source are required");
    }

    if (const m110::Status distinct = validate_distinct_paths(options); !distinct.is_ok())
    {
        put_error(distinct.message);
        return distinct;
    }

    return options;
}

// ---- reference level ----

// The RMS of the unscaled source over the scenario's reference (E.5.5 refers SNR to
// the input power): "auto" trims leading and trailing exact zeros, an interval is
// [start, start + duration). Reads the source once; none for a silent reference.
[[nodiscard]] m110::Result<double> measure_reference(const Options& options, const sl::Scenario& scenario)
{
    SampleSource source;

    if (const m110::Status opened = source.open(options.source); !opened.is_ok())
    {
        return opened;
    }

    const std::uint64_t first = scenario.reference_auto ? 0U : sl::det::seconds_to_frames(scenario.reference_start_seconds, sl::sample_rate_hz);
    const std::uint64_t last = scenario.reference_auto ? source.frames() : first + sl::det::seconds_to_frames(scenario.reference_duration_seconds, sl::sample_rate_hz);

    if (last > source.frames())
    {
        return refused("reference interval extends beyond the source");
    }

    std::vector<float> buffer(prepass_frames);
    double energy = 0.0;
    std::uint64_t frame = 0U;
    std::optional<std::uint64_t> first_nonzero{};
    std::uint64_t last_nonzero = 0U;

    for (;;)
    {
        const m110::Result<std::size_t> count = source.read(buffer);

        if (!count.has_value())
        {
            return count.error();
        }

        if (count.value() == 0U)
        {
            break;
        }

        for (const float sample : std::span<const float>{buffer}.first(count.value()))
        {
            if (frame >= first && frame < last)
            {
                energy += sl::as_double(sample) * sl::as_double(sample);
            }

            if (sample != 0.0F)
            {
                first_nonzero = first_nonzero.value_or(frame);
                last_nonzero = frame;
            }

            ++frame;
        }
    }

    // "auto": zeros contribute no energy, so the trimmed interval only changes the count.
    const std::uint64_t begin = scenario.reference_auto ? first_nonzero.value_or(0U) : first;
    const std::uint64_t end = scenario.reference_auto ? (first_nonzero.has_value() ? last_nonzero + 1U : 0U) : last;

    if (end <= begin || energy <= 0.0)
    {
        return refused("the reference interval is silent");
    }

    return std::sqrt(energy / sl::as_double(end - begin));
}

// ---- sidecar JSON ----

// Sixteen lower-case hexadecimal digits, as the engine's digests are printed elsewhere.
[[nodiscard]] std::string hex_text(std::uint64_t value)
{
    std::array<char, hex_digits> digits{};
    const auto written = std::to_chars(digits.data(), std::to_address(digits.end()), value, hex_base);
    const std::string_view text{digits.data(), written.ptr};
    return std::string(hex_digits - text.size(), '0') + std::string{text};
}

class Json
{
public:
    void member(std::string_view name)
    {
        separator();
        string(name);
        mText += ": ";
    }

    void string(std::string_view value)
    {
        mText += '"';

        for (const char letter : value)
        {
            if (letter == '"' || letter == '\\')
            {
                mText += '\\';
            }

            mText += letter;
        }

        mText += '"';
    }

    void number(double value)
    {
        if (!std::isfinite(value))
        {
            mText += "null";
            return;
        }

        std::array<char, double_characters> digits{};
        const auto written = std::to_chars(digits.data(), std::to_address(digits.end()), value);
        mText.append(digits.data(), written.ptr);
    }

    void integer(std::uint64_t value) { mText += std::to_string(value); }

    void hex(std::uint64_t value) { string(hex_text(value)); }

    void open(char bracket)
    {
        mText += bracket;
        mFirst = true;
    }

    void close(char bracket)
    {
        mText += bracket;
        mFirst = false;
    }

    // Starts an element of an array.
    void element() { separator(); }

    [[nodiscard]] const std::string& text() const noexcept { return mText; }

private:
    void separator()
    {
        if (!mFirst)
        {
            mText += ",\n ";
        }

        mFirst = false;
    }

    std::string mText{};
    bool mFirst{true};
};

[[nodiscard]] std::string_view fading_name(sl::PathFading fading) noexcept
{
    return fading == sl::PathFading::rayleigh ? "rayleigh" : "fixed";
}

void write_path(Json& json, const sl::WattersonPathReport& path)
{
    json.element();
    json.open('{');
    json.member("fading");
    json.string(fading_name(path.fading));
    json.member("delay_frames");
    json.integer(path.delay.value());
    json.member("frames_per_node");
    json.integer(path.frames_per_node.value());
    json.member("node_rate_hz");
    json.number(path.node_rate.in(units::Hz));
    json.member("rate_multiple");
    json.number(path.rate_multiple);
    json.member("doppler_taps");
    json.integer(path.doppler_taps);
    json.member("amplitude");
    json.number(path.amplitude);
    json.member("interpolation_power");
    json.number(path.interpolation_power);
    json.member("gain_db");
    json.number(path.gain.in(units::dB));
    json.member("phase_step");
    json.integer(path.phase_step.word());
    json.member("realised_shift_hz");
    json.number(path.realised_shift.in(units::Hz));
    json.member("phase_deg");
    json.number(path.phase.in(units::degrees));
    json.close('}');
}

void write_watterson(Json& json, const sl::WattersonReport* report)
{
    json.member("watterson");

    if (report == nullptr)
    {
        json.string("none");
        return;
    }

    json.open('{');
    json.member("algorithm_version");
    json.integer(sl::watterson_algorithm_version);
    json.member("path_count");
    json.integer(report->path_count);
    json.member("hilbert_taps");
    json.integer(report->hilbert_taps);
    json.member("latency_frames");
    json.integer(report->latency.value());
    json.member("parameter_digest");
    json.hex(report->parameter_digest);
    json.member("paths");
    json.open('[');

    for (const sl::WattersonPathReport& path : std::span{report->paths}.first(report->path_count))
    {
        write_path(json, path);
    }

    json.close(']');
    json.close('}');
}

void write_cw_pulse(Json& json, const sl::Stage& stage)
{
    if (stage.type != sl::StageType::cw || stage.cw.pulse_period_frames == 0U)
    {
        return;
    }

    json.member("pulse_period_frames");
    json.integer(stage.cw.pulse_period_frames);
    json.member("pulse_on_frames");
    json.integer(stage.cw.pulse_on_frames);
    json.member("pulse_realized_duty_cycle");
    json.number(static_cast<double>(stage.cw.pulse_on_frames) / static_cast<double>(stage.cw.pulse_period_frames));
}

void write_cw_sweep(Json& json, const sl::Stage& stage)
{
    if (stage.type != sl::StageType::cw || !stage.cw.sweep.enabled)
    {
        return;
    }

    const auto& sweep = stage.cw.sweep;
    const std::uint64_t step = sl::cw_sweep_cycle_step(sweep);
    json.member("sweep_algorithm_version");
    json.integer(sl::cw_sweep_algorithm_version);
    json.member("frequency_sweep");
    json.open('{');
    json.member("low_hz");
    json.number(sweep.low.in(sl::units::Hz));
    json.member("high_hz");
    json.number(sweep.high.in(sl::units::Hz));
    json.member("slope_hz_per_second");
    json.number(sweep.slope.hertz_per_second());
    json.member("cycle_step_q64");
    json.integer(step);
    json.member("realized_period_seconds");
    json.number(18446744073709551616.0 / static_cast<double>(step) / sl::sample_rate_hz);
    json.close('}');
}

void write_cw_markov(Json& json, const sl::Stage& stage)
{
    if (stage.type != sl::StageType::cw || !stage.cw.markov.enabled)
    {
        return;
    }

    const auto& gate = stage.cw.markov;
    json.member("markov_on_off");
    json.open('{');
    json.member("mean_on_seconds");
    json.number(gate.mean_on_seconds);
    json.member("mean_off_seconds");
    json.number(gate.mean_off_seconds);
    json.member("mean_on_frames");
    json.number(gate.mean_on_seconds * sl::sample_rate_hz);
    json.member("mean_off_frames");
    json.number(gate.mean_off_seconds * sl::sample_rate_hz);
    json.member("initial_state");
    json.string("on");
    json.member("dwell_distribution");
    json.string("geometric");
    json.member("rng_family");
    json.integer(sl::markov_gate_rng_family);
    json.member("algorithm_version");
    json.integer(sl::markov_gate_algorithm_version);
    json.close('}');
}

void write_cw_fsk(Json& json, const sl::Stage& stage)
{
    if (stage.type != sl::StageType::cw || !stage.cw.fsk.enabled)
    {
        return;
    }

    const auto& fsk = stage.cw.fsk;
    const double shift = fsk.shift.in(sl::units::Hz);
    // Engine configuration already validated this exact symbol period.
    const auto frames = sl::cw_fsk_symbol_frames(fsk);
    json.member("fsk");
    json.open('{');
    json.member("baud");
    json.number(fsk.baud);
    json.member("shift_hz");
    json.number(shift);
    json.member("mark_hz");
    json.number(stage.cw.frequency_hz + shift / 2.0);
    json.member("space_hz");
    json.number(stage.cw.frequency_hz - shift / 2.0);
    json.member("frames_per_symbol");
    json.integer(frames.value());
    json.member("pattern");
    json.string(fsk.pattern == sl::FskPattern::alternating ? "alternating" : "random");
    json.member("rng_family");
    json.integer(sl::cw_fsk_rng_family);
    json.member("algorithm_version");
    json.integer(sl::cw_fsk_algorithm_version);
    json.close('}');
}

void write_cw_fading(Json& json, const sl::Stage& stage)
{
    if (stage.type != sl::StageType::cw || !stage.cw.fading.enabled)
    {
        return;
    }

    json.member("fading");
    json.open('{');
    json.member("spread_hz");
    json.number(stage.cw.fading.spread.in(sl::units::Hz));
    json.member("mean_power");
    json.number(1.0);
    json.member("distribution");
    json.string("rayleigh");
    json.member("rng_family");
    json.integer(sl::cw_fading_rng_family);
    json.member("algorithm_version");
    json.integer(sl::cw_fading_algorithm_version);
    json.member("rng_implementation");
    json.string("det-pcg32");
    json.close('}');
}

void write_random_key(Json& json, const sl::Scenario& scenario, std::uint32_t index)
{
    const std::uint32_t stream_id = scenario.stages[index].stream_id;

    if (stream_id == 0U)
    {
        return;
    }

    const auto key = sl::resolve_stage_random_key(scenario.seed, index, stream_id);
    json.member("random_key");
    json.open('{');
    json.member("mode");
    json.string("stream-id-v1");
    json.member("stream_id");
    json.integer(stream_id);
    json.member("effective_seed_hex");
    json.hex(key.seed);
    json.member("index");
    json.integer(0U);
    json.member("algorithm_version");
    json.integer(sl::stream_identity_algorithm_version);
    json.close('}');
}

void write_stages(Json& json, const sl::Engine& engine, const sl::Scenario& scenario)
{
    json.member("stages");
    json.open('[');

    for (std::uint32_t index = 0U; index < scenario.stage_count; ++index)
    {
        const sl::Impairment* const stage = engine.stage(index);
        const sl::StageStats& stats = stage->stats();
        json.element();
        json.open('{');
        json.member("order");
        json.integer(index);
        json.member("type");
        json.string(sl::stage_type_name(stage->type()));
        json.member("component_energy");
        json.number(stats.component_energy);
        json.member("component_frames");
        json.integer(stats.component_frames);
        json.member("component_peak");
        json.number(sl::as_double(stats.component_peak));
        write_cw_pulse(json, scenario.stages[index]);
        write_cw_sweep(json, scenario.stages[index]);
        write_cw_markov(json, scenario.stages[index]);
        write_cw_fsk(json, scenario.stages[index]);
        write_cw_fading(json, scenario.stages[index]);
        write_random_key(json, scenario, index);
        json.close('}');
    }

    json.close(']');
}

struct RenderFacts
{
    double reference_rms{};
    std::uint64_t truth_records{};
    double noise_band_enbw_hz{};
};

[[nodiscard]] std::string sidecar_text(const Options& options, const sl::Scenario& scenario, const sl::Engine& engine, const RenderFacts& facts)
{
    const sl::EngineStats& stats = engine.stats();
    Json json;
    json.open('{');
    json.member("schema");
    json.string(sidecar_schema);
    json.member("tool");
    json.string(tool_name);
    json.member("tool_version");
    json.string(tool_version);
    json.member("scenario_path");
    json.string(options.scenario);
    json.member("source_path");
    json.string(options.source.empty() ? std::string_view{"silence"} : std::string_view{options.source});
    json.member("output_path");
    json.string(options.output);
    json.member("truth_path");
    json.string(options.truth);
    json.member("truth_records");
    json.integer(facts.truth_records);
    json.member("seed");
    json.integer(scenario.seed);
    json.member("block_frames");
    json.integer(options.block);
    json.member("source_gain_db");
    json.number(scenario.source_gain_db);
    json.member("reference_rms");
    json.number(facts.reference_rms);
    json.member("scaled_reference_rms");
    json.number(engine.scaled_reference_rms());
    json.member("noise_band_enbw_hz");
    json.number(facts.noise_band_enbw_hz);
    const double bandwidth = sl::noise_band_width(scenario.noise_bandwidth).hertz();
    const double low = sl::noise_band_low.hertz();
    json.member("noise_bandwidth_hz");
    json.number(bandwidth);
    json.member("noise_band_low_hz");
    json.number(low);
    json.member("noise_band_high_hz");
    json.number(low + bandwidth);
    json.member("noise_band_center_hz");
    json.number(low + bandwidth / 2.0);
    json.member("frames_in");
    json.integer(stats.frames_in);
    json.member("frames_out");
    json.integer(stats.frames_out);
    json.member("clipped_samples");
    json.integer(stats.clipped_samples);
    json.member("peak");
    json.number(sl::as_double(stats.peak));
    json.member("output_energy");
    json.number(stats.output_energy);
    json.member("output_digest");
    json.hex(stats.output_digest);
    json.member("source_digest");
    json.hex(stats.source_digest);
    write_stages(json, engine, scenario);
    write_watterson(json, engine.watterson_report());
    json.close('}');
    return json.text() + "\n";
}

// Writes `text` as the whole file: success, or why it is not written.
[[nodiscard]] m110::Status write_text(const std::string& path, std::string_view text)
{
    std::ofstream file{path, std::ios::binary | std::ios::trunc};
    const m110::Status written = file ? write_exact(file, text) : unwritable("cannot create the file");
    file.close();
    return written.is_ok() && file.fail() ? failed_write : written;
}

// ---- the render ----

// The engine, the channel and the scenario are large: static storage, as on the board.
struct RenderState
{
    sl::Engine engine;
    sl::WattersonChannel channel{};
    sl::Scenario scenario{};
};

[[nodiscard]] RenderState& state() noexcept
{
    static RenderState instance{};
    return instance;
}

constexpr m110::Status scenario_unreadable{.code = m110::StatusCode::io_error, .message = "cannot read the scenario"};

// The scenario file, parsed, with --seed applied: success, scenario_unreadable,
// or parse_scenario()'s Status.
[[nodiscard]] m110::Status load_scenario(const Options& options, sl::Scenario& scenario)
{
    std::ifstream file{options.scenario, std::ios::binary};

    if (!file)
    {
        return scenario_unreadable;
    }

    const std::string text{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};

    if (const m110::Status parsed = sl::parse_scenario(text.data(), text.size(), scenario); !parsed.is_ok())
    {
        return parsed;
    }

    scenario.seed = options.seed.value_or(scenario.seed);
    return m110::Status::success();
}

// What load_scenario()'s refusal prints: a parse error after "scenario error: ".
[[nodiscard]] std::string scenario_failure(m110::Status failure)
{
    return sl::same_failure(failure, scenario_unreadable) ? std::string{failure.message} : std::string{"scenario error: "} + failure.message;
}

[[nodiscard]] m110::Result<double> reference_level(const Options& options, const sl::Scenario& scenario)
{
    if (options.reference_rms.has_value())
    {
        return *options.reference_rms;
    }

    if (scenario.has_reference_rms)
    {
        return scenario.reference_rms;
    }

    if (options.source.empty())
    {
        return refused("silence needs --reference-rms or the scenario's reference_rms");
    }

    return measure_reference(options, scenario);
}

[[nodiscard]] m110::Status open_source(const Options& options, SampleSource& source)
{
    if (options.silence_seconds.has_value())
    {
        source.open_silence(sl::det::seconds_to_frames(*options.silence_seconds, sl::sample_rate_hz));
        return m110::Status::success();
    }

    return source.open(options.source);
}

// Why the engine stopped, from the Status it returned: a stage fault carries the
// stage's own static message, the engine's own failures theirs.
[[nodiscard]] std::string process_failure(m110::Status failure)
{
    if (sl::same_failure(failure, sl::engine_failure::clipping))
    {
        return "render rejected: clipping (clip_policy reject)";
    }

    if (sl::same_failure(failure, sl::engine_failure::nonfinite_input))
    {
        return "render rejected: a non-finite source sample";
    }

    return std::string{sl::is_engine_failure(failure) ? "engine processing failed: " : "stage fault: "} + failure.message;
}

// Streams the source through the engine into the writer, block by block.
[[nodiscard]] int stream(const Options& options, const sl::Scenario& scenario, sl::Engine& engine, SampleSource& source, Pcm24Writer& writer, FrequencyTruthWriter* frequency_truth)
{
    std::vector<float> input(options.block);
    std::vector<float> output(sl::engine_capacity_frames);
    std::uint64_t first_input_frame = 0U;

    for (;;)
    {
        const m110::Result<std::size_t> count = source.read(input);

        if (!count.has_value())
        {
            put_error("cannot read " + options.source);
            return exit_file;
        }

        if (count.value() == 0U)
        {
            return exit_ok;
        }

        const m110::Result<std::size_t> produced = engine.process(std::span<const float>{input}.first(count.value()), output);

        if (!produced.has_value())
        {
            put_error(process_failure(produced.error()));
            return exit_usage;
        }

        if (frequency_truth != nullptr)
        {
            const m110::Status truth_written = frequency_truth->write(scenario.frequency_shift, first_input_frame, count.value());

            if (!truth_written.is_ok())
            {
                if (sl::same_failure(truth_written, frequency_truth_order) || sl::same_failure(truth_written, frequency_truth_range))
                {
                    put_error(truth_written.message);
                    return exit_usage;
                }

                put_error("cannot write " + options.frequency_truth);
                return exit_file;
            }
        }

        if (!writer.write(std::span<const float>{output}.first(produced.value())).is_ok())
        {
            put_error("cannot write " + options.output);
            return exit_file;
        }

        first_input_frame += static_cast<std::uint64_t>(count.value());
    }
}

[[nodiscard]] int render(const Options& options)
{
    RenderState& render_state = state();
    SampleSource source;

    if (const m110::Status loaded = load_scenario(options, render_state.scenario); !loaded.is_ok())
    {
        put_error(scenario_failure(loaded));
        return exit_usage;
    }

    if (scenario_has_stage(render_state.scenario, sl::StageType::second_source))
    {
        put_error("second_source requires signal_lab_render; this validation driver has no secondary source provider");
        return exit_usage;
    }

    const bool has_frequency_shift = scenario_has_stage(render_state.scenario, sl::StageType::frequency_shift);

    if (!options.frequency_truth.empty() && !has_frequency_shift)
    {
        put_error("--frequency-truth-out requires a frequency_shift scenario stage");
        return exit_usage;
    }

    if (const m110::Status opened = open_source(options, source); !opened.is_ok())
    {
        put_error(opened.message);
        return exit_usage;
    }

    const m110::Result<double> reference = reference_level(options, render_state.scenario);

    if (!reference.has_value())
    {
        put_error(reference.error().message);
        return exit_usage;
    }

    const auto selected_kernel = sl::noise_band_kernel(render_state.scenario.noise_bandwidth);

    if (!selected_kernel.has_value())
    {
        put_error(selected_kernel.status().message);
        return exit_usage;
    }

    TruthWriter truth;
    const bool with_truth = !options.truth.empty();

    if (with_truth && !truth.open(options.truth).is_ok())
    {
        put_error("cannot write " + options.truth);
        return exit_file;
    }

    const sl::StageResources resources{&render_state.channel, with_truth ? &truth : nullptr};

    if (const m110::Status configured = render_state.engine.configure(render_state.scenario, reference.value(), source.frames(), resources); !configured.is_ok())
    {
        put_error(std::string{"engine configuration failed: "} + configured.message);
        return exit_usage;
    }

    FrequencyTruthWriter frequency_truth;
    const bool with_frequency_truth = !options.frequency_truth.empty();

    if (with_frequency_truth &&
        !frequency_truth.open(options.frequency_truth, render_state.scenario.frequency_shift, scenario_has_stage(render_state.scenario, sl::StageType::watterson), source.frames()).is_ok())
    {
        put_error("cannot write " + options.frequency_truth);
        return exit_file;
    }

    Pcm24Writer writer;

    if (!writer.open(options.output).is_ok())
    {
        put_error("cannot write " + options.output);

        if (with_frequency_truth && !frequency_truth.close().is_ok())
        {
            put_error("cannot finish " + options.frequency_truth);
        }

        return exit_file;
    }

    const int streamed = stream(options, render_state.scenario, render_state.engine, source, writer, with_frequency_truth ? &frequency_truth : nullptr);
    const m110::Status output_closed = writer.close();
    const m110::Status truth_closed = with_truth ? truth.close() : m110::Status::success();
    const m110::Status frequency_truth_closed = with_frequency_truth ? frequency_truth.close() : m110::Status::success();
    const bool closed = output_closed.is_ok() && truth_closed.is_ok() && frequency_truth_closed.is_ok();

    if (streamed != exit_ok || !closed)
    {
        const std::string failed_path = !output_closed.is_ok() ? options.output : !truth_closed.is_ok() ? options.truth : options.frequency_truth;
        put_error(closed ? std::string{"render failed"} : "cannot finish " + failed_path);
        return !closed ? exit_file : streamed;
    }

    const RenderFacts facts{reference.value(), truth.records(), selected_kernel.value().get().enbw.hertz()};

    if (!options.sidecar.empty() && !write_text(options.sidecar, sidecar_text(options, render_state.scenario, render_state.engine, facts)).is_ok())
    {
        put_error("cannot write " + options.sidecar);
        return exit_file;
    }

    const sl::EngineStats& stats = render_state.engine.stats();
    put_out("output=" + options.output + " frames=" + std::to_string(stats.frames_out) + " clipped=" + std::to_string(stats.clipped_samples) + " digest=" + hex_text(stats.output_digest));
    return exit_ok;
}

} // namespace

int main(int argc, char** argv)
{
    m110::install_host_critical_error_handler("wfg_watterson_render", exit_usage);
    const m110::Result<Options> options = parse_arguments(std::span<char*>{argv, argument_count(argc)});

    if (!options.has_value())
    {
        return exit_usage;
    }

    return render(options.value());
}
