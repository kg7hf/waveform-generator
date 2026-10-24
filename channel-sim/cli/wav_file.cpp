// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

#include "cli/wav_file.hpp"

#include <algorithm>
#include <bit>
#include <limits>
#include <span>
#include <string_view>
#include <utility>

namespace channel_sim::cli
{

namespace
{
using m110::Status;
using m110::StatusCode;

using Bytes = std::span<const char>;
using MutableBytes = std::span<char>;

constexpr std::size_t riff_header_bytes = 12U;
constexpr std::size_t riff_form_offset = 8U;
constexpr std::size_t chunk_header_bytes = 8U;
constexpr std::size_t chunk_size_offset = 4U;
constexpr std::size_t format_bytes = 16U;
constexpr unsigned byte_shift = 8U;
constexpr std::uint32_t byte_mask = 0xFFU;

// "fmt " chunk fields.
constexpr std::size_t fmt_tag = 0U;
constexpr std::size_t fmt_channels = 2U;
constexpr std::size_t fmt_rate = 4U;
constexpr std::size_t fmt_block_align = 12U;
constexpr std::size_t fmt_bits = 14U;

// The canonical 44-byte header.
constexpr std::size_t canonical_header_bytes = 44U;
constexpr std::size_t header_riff_size = 4U;
constexpr std::size_t header_wave_fmt = 8U;
constexpr std::size_t header_fmt_size = 16U;
constexpr std::size_t header_format = 20U;
constexpr std::size_t header_channels = 22U;
constexpr std::size_t header_rate = 24U;
constexpr std::size_t header_byte_rate = 28U;
constexpr std::size_t header_block_align = 32U;
constexpr std::size_t header_bits = 34U;
constexpr std::size_t header_data = 36U;
constexpr std::size_t header_data_size = 40U;
constexpr std::uint32_t canonical_riff_overhead = 36U; // "WAVE" + fmt chunk + data chunk header
constexpr std::uint32_t pcm_format_chunk_bytes = 16U;

constexpr std::uint16_t format_pcm = 1U;
constexpr std::uint16_t format_float = 3U;
constexpr std::uint16_t pcm16_bits = 16U;
constexpr std::uint16_t pcm24_bits = 24U;
constexpr std::uint16_t float32_bits = 32U;
constexpr std::uint16_t bits_per_byte = 8U;
constexpr std::uint16_t pcm16_bytes = 2U;
constexpr std::uint16_t max_channels = 2U;
constexpr std::uint32_t pcm24_sign_bit = 0x00800000U;
constexpr std::uint32_t pcm24_sign_extension = 0xFF000000U;

[[nodiscard]] std::uint32_t byte_at(Bytes bytes, std::size_t offset) noexcept
{
    return static_cast<std::uint8_t>(bytes[offset]);
}

[[nodiscard]] std::uint16_t le16(Bytes bytes, std::size_t offset) noexcept
{
    return static_cast<std::uint16_t>(byte_at(bytes, offset) | (byte_at(bytes, offset + 1U) << byte_shift));
}

[[nodiscard]] std::uint32_t le24(Bytes bytes, std::size_t offset) noexcept
{
    return byte_at(bytes, offset) | (byte_at(bytes, offset + 1U) << byte_shift) | (byte_at(bytes, offset + 2U) << (2U * byte_shift));
}

[[nodiscard]] std::uint32_t le32(Bytes bytes, std::size_t offset) noexcept
{
    return le24(bytes, offset) | (byte_at(bytes, offset + 3U) << (3U * byte_shift));
}

[[nodiscard]] bool tag_is(Bytes bytes, std::size_t offset, std::string_view tag) noexcept
{
    return std::ranges::equal(bytes.subspan(offset, tag.size()), tag);
}

[[nodiscard]] char low_byte(std::uint32_t value) noexcept
{
    return static_cast<char>(static_cast<std::uint8_t>(value & byte_mask));
}

void put_le16(MutableBytes bytes, std::size_t offset, std::uint16_t value) noexcept
{
    bytes[offset] = low_byte(value);
    bytes[offset + 1U] = low_byte(static_cast<std::uint32_t>(value) >> byte_shift);
}

void put_le32(MutableBytes bytes, std::size_t offset, std::uint32_t value) noexcept
{
    put_le16(bytes, offset, static_cast<std::uint16_t>(value));
    put_le16(bytes, offset + 2U, static_cast<std::uint16_t>(value >> (2U * byte_shift)));
}

void put_tag(MutableBytes bytes, std::size_t offset, std::string_view tag) noexcept
{
    std::ranges::copy(tag, bytes.subspan(offset, tag.size()).begin());
}

// Reads exactly destination.size() bytes. The end of the file first is a malformed
// input (truncated says which part); a failing stream is an I/O error.
[[nodiscard]] Status read_exact(std::ifstream& file, MutableBytes destination, const char* truncated) noexcept
{
    const auto wanted = static_cast<std::streamsize>(destination.size());
    file.read(destination.data(), wanted);

    if (file.gcount() == wanted)
    {
        return Status::success();
    }

    return file.bad() ? Status{StatusCode::io_error, "cannot read the input file"} : Status{StatusCode::invalid_argument, truncated};
}

// The read position, as a byte offset from the start of the file.
[[nodiscard]] m110::Result<std::uint64_t> position(std::ifstream& file) noexcept
{
    const std::streamoff offset = file.tellg();

    if (file.fail() || offset < 0)
    {
        return Status{StatusCode::io_error, "cannot tell the input file position"};
    }

    return static_cast<std::uint64_t>(offset);
}

// Moves the read position bytes forward. The caller has checked that they lie
// inside the file (WavFileSource::check_inside), so the offset fits a streamoff.
[[nodiscard]] Status skip(std::ifstream& file, std::uint64_t bytes) noexcept
{
    if (!std::in_range<std::streamoff>(bytes))
    {
        return {StatusCode::invalid_argument, "input WAV chunk is too large to skip"};
    }

    file.seekg(static_cast<std::streamoff>(bytes), std::ios::cur);
    return file.fail() ? Status{StatusCode::io_error, "cannot seek in the input file"} : Status::success();
}

// A RIFF chunk's size with its pad byte (odd sizes are padded to even), in 64
// bits: a size of 0xFFFFFFFF must not wrap to 0.
[[nodiscard]] constexpr std::uint64_t padded_size(std::uint32_t size) noexcept
{
    return std::uint64_t{size} + (size & 1U);
}

static_assert(padded_size(std::numeric_limits<std::uint32_t>::max()) == std::uint64_t{std::numeric_limits<std::uint32_t>::max()} + 1U, "the pad byte never wraps the size");

[[nodiscard]] m110::Result<WavEncoding> encoding_of(std::uint16_t tag, std::uint16_t bits) noexcept
{
    if (tag == format_pcm && bits == pcm16_bits)
    {
        return WavEncoding::pcm16;
    }

    if (tag == format_pcm && bits == pcm24_bits)
    {
        return WavEncoding::pcm24;
    }

    if (tag == format_float && bits == float32_bits)
    {
        return WavEncoding::float32;
    }

    return Status{StatusCode::invalid_argument, "input WAV must be PCM16, PCM24 or float32"};
}

[[nodiscard]] double decode_unit(Bytes frame, WavEncoding encoding) noexcept
{
    switch (encoding)
    {
    case WavEncoding::pcm16:
        return pcm16_to_unit(std::bit_cast<Pcm16>(le16(frame, 0U)));

    case WavEncoding::pcm24:
    {
        std::uint32_t raw = le24(frame, 0U);

        if ((raw & pcm24_sign_bit) != 0U)
        {
            raw |= pcm24_sign_extension;
        }

        return pcm24_to_unit(std::bit_cast<std::int32_t>(raw));
    }

    case WavEncoding::float32:
        return static_cast<double>(std::bit_cast<float>(le32(frame, 0U)));
    }

    return 0.0;
}
} // namespace

Status WavFileSource::open(const char* path) noexcept
{
    file_.open(path, std::ios::binary);

    if (!file_.is_open())
    {
        return {StatusCode::io_error, "cannot open the input file"};
    }

    file_.seekg(0, std::ios::end);
    const m110::Result<std::uint64_t> end = position(file_);

    if (!end)
    {
        return end.status();
    }

    file_bytes_ = end.value();
    file_.seekg(0, std::ios::beg);
    std::array<char, riff_header_bytes> riff{};
    const Status header = read_exact(file_, riff, "input is not a RIFF/WAVE file");

    if (!header.is_ok())
    {
        return header;
    }

    if (!tag_is(riff, 0U, "RIFF") || !tag_is(riff, riff_form_offset, "WAVE"))
    {
        return {StatusCode::invalid_argument, "input is not a RIFF/WAVE file"};
    }

    return find_data();
}

const WavFormat& WavFileSource::format() const noexcept
{
    return format_;
}

m110::Result<std::size_t> WavFileSource::read(std::span<Pcm16> destination) noexcept
{
    if (format_.encoding != WavEncoding::pcm16)
    {
        return Status{StatusCode::invalid_configuration, "input is not PCM16"};
    }

    const m110::Result<std::size_t> frames = read_frames(destination.size());

    if (!frames)
    {
        return frames;
    }

    const Bytes bytes{buffer_};

    for (std::size_t frame = 0U; frame < frames.value(); ++frame)
    {
        destination[frame] = std::bit_cast<Pcm16>(le16(bytes, frame * format_.frame_bytes));
    }

    return frames;
}

m110::Result<std::size_t> WavFileSource::read_unit(std::span<double> destination) noexcept
{
    const m110::Result<std::size_t> frames = read_frames(destination.size());

    if (!frames)
    {
        return frames;
    }

    const Bytes bytes{buffer_};

    for (std::size_t frame = 0U; frame < frames.value(); ++frame)
    {
        destination[frame] = decode_unit(bytes.subspan(frame * format_.frame_bytes, format_.frame_bytes), format_.encoding);
    }

    return frames;
}

Status WavFileSource::rewind() noexcept
{
    file_.clear();
    file_.seekg(data_start_);

    if (!file_.is_open() || file_.fail())
    {
        return {StatusCode::io_error, "cannot rewind the input file"};
    }

    frames_left_ = format_.frames;
    return Status::success();
}

// Walks the chunks to "data", reading "fmt " on the way. Every chunk must lie
// inside the file.
Status WavFileSource::find_data() noexcept
{
    for (;;)
    {
        std::array<char, chunk_header_bytes> chunk{};
        const Status header = read_exact(file_, chunk, "input WAV has no data chunk");

        if (!header.is_ok())
        {
            return header;
        }

        const std::uint32_t size = le32(chunk, chunk_size_offset);

        if (tag_is(chunk, 0U, "data"))
        {
            return accept_data(size);
        }

        Status walked = check_inside(padded_size(size), "input WAV has a chunk that runs past the end of the file");

        if (walked.is_ok())
        {
            walked = tag_is(chunk, 0U, "fmt ") ? read_format(size) : skip(file_, padded_size(size));
        }

        if (!walked.is_ok())
        {
            return walked;
        }
    }
}

// Fails unless bytes more bytes follow the read position in the file.
Status WavFileSource::check_inside(std::uint64_t bytes, const char* beyond) noexcept
{
    const m110::Result<std::uint64_t> at = position(file_);

    if (!at)
    {
        return at.status();
    }

    return at.value() <= file_bytes_ && bytes <= file_bytes_ - at.value() ? Status::success() : Status{StatusCode::invalid_argument, beyond};
}

// The chunk's padded size is inside the file (find_data checked it).
Status WavFileSource::read_format(std::uint32_t chunk_bytes) noexcept
{
    if (chunk_bytes < format_bytes)
    {
        return {StatusCode::invalid_argument, "input WAV has a short fmt chunk"};
    }

    std::array<char, format_bytes> fmt{};
    const Status read = read_exact(file_, fmt, "input WAV has a short fmt chunk");

    if (!read.is_ok())
    {
        return read;
    }

    const Status skipped = skip(file_, padded_size(chunk_bytes) - format_bytes);

    if (!skipped.is_ok())
    {
        return skipped;
    }

    format_tag_ = le16(fmt, fmt_tag);
    format_.channels = le16(fmt, fmt_channels);
    format_.rate = SampleRate{le32(fmt, fmt_rate)};
    format_.frame_bytes = le16(fmt, fmt_block_align);
    format_.bits = le16(fmt, fmt_bits);
    have_format_ = true;
    return Status::success();
}

Status WavFileSource::accept_data(std::uint32_t chunk_bytes) noexcept
{
    if (!have_format_)
    {
        return {StatusCode::invalid_argument, "input WAV has data before fmt"};
    }

    const m110::Result<WavEncoding> encoding = encoding_of(format_tag_, format_.bits);

    if (!encoding)
    {
        return encoding.status();
    }

    format_.encoding = encoding.value();

    if (format_.channels == 0U || format_.channels > max_channels || format_.rate.hz == 0U || format_.frame_bytes != format_.channels * (format_.bits / bits_per_byte))
    {
        return {StatusCode::invalid_argument, "input WAV must be mono or stereo with a consistent block size"};
    }

    const Status inside = check_inside(chunk_bytes, "input WAV is shorter than its data chunk");

    if (!inside.is_ok())
    {
        return inside;
    }

    format_.frames = chunk_bytes / format_.frame_bytes;
    frames_left_ = format_.frames;
    data_start_ = file_.tellg();
    return file_.fail() ? Status{StatusCode::io_error, "cannot record the input data position"} : Status::success();
}

m110::Result<std::size_t> WavFileSource::read_frames(std::size_t wanted) noexcept
{
    if (!file_.is_open() || format_.frame_bytes == 0U)
    {
        return Status{StatusCode::invalid_configuration, "input file is not open"};
    }

    const std::uint64_t capacity = buffer_.size() / format_.frame_bytes;
    const auto frames = static_cast<std::size_t>(std::min({static_cast<std::uint64_t>(wanted), capacity, frames_left_}));

    if (frames > 0U)
    {
        const Status read = read_exact(file_, std::span<char>{buffer_}.first(frames * format_.frame_bytes), "input WAV is shorter than its data chunk");

        if (!read.is_ok())
        {
            return read;
        }
    }

    frames_left_ -= frames;
    return frames;
}

WavUnitSource::WavUnitSource(WavFileSource& file) noexcept : file_{&file} {}

m110::Result<std::size_t> WavUnitSource::read(std::span<double> destination) noexcept
{
    return file_->read_unit(destination);
}

Status WavUnitSource::rewind() noexcept
{
    return file_->rewind();
}

Status WavFileSink::open(const char* path, SampleRate rate, std::uint16_t channels) noexcept
{
    if (channels == 0U || channels > max_channels || rate.hz == 0U)
    {
        return {StatusCode::invalid_argument, "output must be mono or stereo"};
    }

    file_.open(path, std::ios::binary | std::ios::trunc);

    if (!file_.is_open())
    {
        return {StatusCode::io_error, "cannot create the output file"};
    }

    rate_ = rate;
    channels_ = channels;
    frames_ = 0U;
    buffered_ = 0U;
    return write_header(0U);
}

Status WavFileSink::write(std::span<const Pcm16> samples) noexcept
{
    if (!file_.is_open())
    {
        return {StatusCode::invalid_configuration, "output file is not open"};
    }

    const MutableBytes buffer{buffer_};

    for (const Pcm16 sample : samples)
    {
        for (std::uint16_t channel = 0U; channel < channels_; ++channel)
        {
            if (buffered_ + pcm16_bytes > buffer.size())
            {
                const Status flushed = flush();

                if (!flushed.is_ok())
                {
                    return flushed;
                }
            }

            put_le16(buffer, buffered_, std::bit_cast<std::uint16_t>(sample));
            buffered_ += pcm16_bytes;
        }

        ++frames_;
    }

    return Status::success();
}

Status WavFileSink::finish() noexcept
{
    if (!file_.is_open())
    {
        return {StatusCode::invalid_configuration, "output file is not open"};
    }

    const Status flushed = flush();

    if (!flushed.is_ok())
    {
        return flushed;
    }

    const std::uint64_t data_bytes = frames_ * channels_ * pcm16_bytes;

    if (data_bytes > std::numeric_limits<std::uint32_t>::max() - canonical_riff_overhead)
    {
        return {StatusCode::invalid_argument, "output exceeds the 4 GB WAV limit"};
    }

    file_.seekp(0);
    const Status header = write_header(static_cast<std::uint32_t>(data_bytes));
    file_.close();

    if (!header.is_ok())
    {
        return header;
    }

    return file_.fail() ? Status{StatusCode::io_error, "cannot close the output file"} : Status::success();
}

// The canonical header both pathsim (AudioFile.h) and the lane resampler write.
Status WavFileSink::write_header(std::uint32_t data_bytes) noexcept
{
    const auto block_align = static_cast<std::uint16_t>(channels_ * pcm16_bytes);
    std::array<char, canonical_header_bytes> header{};
    put_tag(header, 0U, "RIFF");
    put_le32(header, header_riff_size, canonical_riff_overhead + data_bytes);
    put_tag(header, header_wave_fmt, "WAVEfmt ");
    put_le32(header, header_fmt_size, pcm_format_chunk_bytes);
    put_le16(header, header_format, format_pcm);
    put_le16(header, header_channels, channels_);
    put_le32(header, header_rate, rate_.hz);
    put_le32(header, header_byte_rate, rate_.hz * block_align);
    put_le16(header, header_block_align, block_align);
    put_le16(header, header_bits, pcm16_bits);
    put_tag(header, header_data, "data");
    put_le32(header, header_data_size, data_bytes);
    file_.write(header.data(), static_cast<std::streamsize>(header.size()));
    return file_.fail() ? Status{StatusCode::io_error, "cannot write the output header"} : Status::success();
}

Status WavFileSink::flush() noexcept
{
    if (buffered_ > 0U)
    {
        file_.write(buffer_.data(), static_cast<std::streamsize>(buffered_));
    }

    buffered_ = 0U;
    return file_.fail() ? Status{StatusCode::io_error, "cannot write the output file"} : Status::success();
}

} // namespace channel_sim::cli
