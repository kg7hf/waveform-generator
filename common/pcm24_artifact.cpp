// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

#include "common/pcm24_artifact.hpp"

#include <algorithm>
#include <limits>

namespace waveform_generator::artifact
{
namespace
{

inline constexpr m110::Status artifact_sink_failed{.code = m110::StatusCode::io_error, .message = "PCM24 artifact sink operation failed"};
inline constexpr m110::Status artifact_bad_state{.code = m110::StatusCode::invalid_argument, .message = "PCM24 artifact writer is not active"};
inline constexpr m110::Status artifact_bad_format{.code = m110::StatusCode::invalid_argument, .message = "PCM24 WAV format must be mono or stereo with a valid byte rate"};
inline constexpr m110::Status artifact_bad_frames{.code = m110::StatusCode::invalid_argument, .message = "PCM24 input must contain a bounded whole number of frames"};
inline constexpr m110::Status artifact_too_large{.code = m110::StatusCode::invalid_argument, .message = "PCM24 artifact exceeds canonical RIFF capacity"};

[[nodiscard]] constexpr std::uint32_t little_endian(std::uint32_t value, std::size_t byte) noexcept
{
    return (value >> static_cast<unsigned int>(8U * byte)) & 0xffU;
}

void put16(std::array<std::uint8_t, pcm24_wav_header_bytes>& header, std::size_t offset, std::uint16_t value) noexcept
{
    header[offset] = static_cast<std::uint8_t>(value & 0xffU);
    header[offset + 1U] = static_cast<std::uint8_t>((value >> 8U) & 0xffU);
}

void put32(std::array<std::uint8_t, pcm24_wav_header_bytes>& header, std::size_t offset, std::uint32_t value) noexcept
{
    for (std::size_t index = 0U; index < 4U; ++index)
    {
        header[offset + index] = static_cast<std::uint8_t>(little_endian(value, index));
    }
}

void put_tag(std::array<std::uint8_t, pcm24_wav_header_bytes>& header, std::size_t offset, const char* tag) noexcept
{
    for (std::size_t index = 0U; index < 4U; ++index)
    {
        header[offset + index] = static_cast<std::uint8_t>(tag[index]);
    }
}

} // namespace

m110::Status Pcm24ArtifactWriter::begin(Pcm24ByteSink& sink, Pcm24WavFormat format) noexcept
{
    if (state_ == Pcm24ArtifactState::active)
    {
        return artifact_bad_state;
    }

    sink_ = nullptr;
    format_ = {};
    digest_ = {};
    frames_ = 0U;
    data_bytes_ = 0U;
    failure_ = m110::Status::success();
    state_ = Pcm24ArtifactState::idle;

    if ((format.channels != 1U && format.channels != 2U) || format.sample_rate_hz == 0U || format.sample_rate_hz > std::numeric_limits<std::uint32_t>::max() / (format.channels * 3U))
    {
        return fail(artifact_bad_format);
    }

    sink_ = &sink;
    format_ = format;
    digest_ = {};
    frames_ = 0U;
    data_bytes_ = 0U;
    failure_ = m110::Status::success();
    state_ = Pcm24ArtifactState::active;

    if (const auto positioned = sink_->seek(0U); !positioned.is_ok())
    {
        return fail(positioned);
    }

    const auto header = make_header(0U);

    if (const auto written = sink_->write_exact(header); !written.is_ok())
    {
        return fail(written);
    }

    return m110::Status::success();
}

m110::Status Pcm24ArtifactWriter::append_frames(std::span<const std::uint8_t> packed_pcm24) noexcept
{
    if (state_ != Pcm24ArtifactState::active || sink_ == nullptr)
    {
        return state_ == Pcm24ArtifactState::failed ? failure_ : artifact_bad_state;
    }

    const std::uint64_t bytes_per_frame = static_cast<std::uint64_t>(format_.channels) * 3U;

    if (packed_pcm24.size() % bytes_per_frame != 0U || packed_pcm24.size() / bytes_per_frame > pcm24_artifact_max_append_frames)
    {
        return fail(artifact_bad_frames);
    }

    if (packed_pcm24.empty())
    {
        return m110::Status::success();
    }

    const auto new_bytes = static_cast<std::uint64_t>(data_bytes_) + packed_pcm24.size();
    const auto new_frames = frames_ + packed_pcm24.size() / bytes_per_frame;
    constexpr auto max_data_bytes = std::numeric_limits<std::uint32_t>::max() - pcm24_wav_riff_overhead_bytes;
    const auto riff_size = static_cast<std::uint64_t>(pcm24_wav_riff_overhead_bytes) + new_bytes + (new_bytes & 1U);

    if (new_bytes > max_data_bytes || riff_size > std::numeric_limits<std::uint32_t>::max() || new_frames < frames_)
    {
        return fail(artifact_too_large);
    }

    if (const auto written = sink_->write_exact(packed_pcm24); !written.is_ok())
    {
        return fail(written);
    }

    if (!packed_pcm24.empty())
    {
        digest_.update(packed_pcm24.data(), packed_pcm24.size());
    }

    data_bytes_ = static_cast<std::uint32_t>(new_bytes);
    frames_ = new_frames;
    return m110::Status::success();
}

m110::Status Pcm24ArtifactWriter::finish(Pcm24ArtifactSummary& summary) noexcept
{
    if (state_ != Pcm24ArtifactState::active || sink_ == nullptr)
    {
        return state_ == Pcm24ArtifactState::failed ? failure_ : artifact_bad_state;
    }

    const std::uint64_t final_bytes = static_cast<std::uint64_t>(pcm24_wav_header_bytes) + data_bytes_ + (data_bytes_ & 1U);

    if ((data_bytes_ & 1U) != 0U)
    {
        constexpr std::array<std::uint8_t, 1U> padding{};

        if (const auto padded = sink_->write_exact(padding); !padded.is_ok())
        {
            return fail(padded);
        }
    }

    if (const auto positioned = sink_->seek(0U); !positioned.is_ok())
    {
        return fail(positioned);
    }

    const auto header = make_header(data_bytes_);

    if (const auto written = sink_->write_exact(header); !written.is_ok())
    {
        return fail(written);
    }

    if (const auto positioned = sink_->seek(final_bytes); !positioned.is_ok())
    {
        return fail(positioned);
    }

    Pcm24ArtifactSummary completed{};
    completed.frames = frames_;
    completed.data_bytes = data_bytes_;
    completed.file_bytes = final_bytes;
    char hex_digest[65]{};
    digest_.hex(hex_digest);
    std::ranges::copy(hex_digest, completed.pcm_sha256.begin());
    summary = completed;
    state_ = Pcm24ArtifactState::complete;
    sink_ = nullptr;
    return m110::Status::success();
}

void Pcm24ArtifactWriter::cancel() noexcept
{
    sink_ = nullptr;
    frames_ = 0U;
    data_bytes_ = 0U;
    digest_ = {};
    failure_ = m110::Status::success();
    state_ = Pcm24ArtifactState::cancelled;
}

m110::Status Pcm24ArtifactWriter::fail(m110::Status status) noexcept
{
    failure_ = status.is_ok() ? artifact_sink_failed : status;
    state_ = Pcm24ArtifactState::failed;
    sink_ = nullptr;
    return failure_;
}

std::array<std::uint8_t, pcm24_wav_header_bytes> Pcm24ArtifactWriter::make_header(std::uint32_t data_bytes) const noexcept
{
    std::array<std::uint8_t, pcm24_wav_header_bytes> header{};
    const auto padded_bytes = data_bytes + (data_bytes & 1U);
    put_tag(header, 0U, "RIFF");
    put32(header, 4U, pcm24_wav_riff_overhead_bytes + padded_bytes);
    put_tag(header, 8U, "WAVE");
    put_tag(header, 12U, "fmt ");
    put32(header, 16U, 16U);
    put16(header, 20U, 1U);
    put16(header, 22U, format_.channels);
    put32(header, 24U, format_.sample_rate_hz);
    put32(header, 28U, format_.sample_rate_hz * format_.channels * 3U);
    put16(header, 32U, static_cast<std::uint16_t>(format_.channels * 3U));
    put16(header, 34U, 24U);
    put_tag(header, 36U, "data");
    put32(header, 40U, data_bytes);
    return header;
}

} // namespace waveform_generator::artifact
