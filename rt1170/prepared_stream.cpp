// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

#include "prepared_stream.hpp"

#include <cstring>
#include <limits>

namespace waveform_generator
{
namespace
{

inline constexpr m110::Status stream_not_bound{.code = m110::StatusCode::unavailable, .message = "prepared stream is not bound"};
inline constexpr m110::Status stream_bad_binding{.code = m110::StatusCode::invalid_argument, .message = "prepared stream requires an open file and aligned scratch"};
inline constexpr m110::Status stream_bad_range{.code = m110::StatusCode::invalid_argument, .message = "prepared stream range is invalid"};
inline constexpr m110::Status stream_io_error{.code = m110::StatusCode::io_error, .message = "prepared stream FatFs operation failed"};
inline constexpr m110::Status stream_short_io{.code = m110::StatusCode::io_error, .message = "prepared stream received a short FatFs operation"};

} // namespace

m110::Status Rt1170PreparedStream::bind(FIL* file, const std::span<std::uint8_t> read_scratch) noexcept
{
    if (file_ != nullptr)
    {
        return {.code = m110::StatusCode::busy, .message = "prepared stream is already bound"};
    }

    if (file == nullptr || read_scratch.size() < 4U || (reinterpret_cast<std::uintptr_t>(read_scratch.data()) % 4U) != 0U || f_size(file) != 0U || f_tell(file) != 0U)
    {
        return stream_bad_binding;
    }

    file_ = file;
    read_scratch_ = read_scratch;
    writer_cursor_ = 0U;
    written_extent_ = 0U;
    committed_frames_ = 0U;
    failure_ = m110::Status::success();
    return m110::Status::success();
}

m110::Status Rt1170PreparedStream::write_exact(const std::span<const std::uint8_t> bytes) noexcept
{
    if (file_ == nullptr)
    {
        return stream_not_bound;
    }

    if (!failure_.is_ok())
    {
        return failure_;
    }

    if (bytes.empty())
    {
        return m110::Status::success();
    }

    if (bytes.size() > std::numeric_limits<UINT>::max() || bytes.size() > std::numeric_limits<std::uint64_t>::max() - writer_cursor_ ||
        writer_cursor_ + bytes.size() > std::numeric_limits<FSIZE_t>::max())
    {
        return stream_bad_range;
    }

    const std::uint64_t new_cursor = writer_cursor_ + bytes.size();
    constexpr std::uint64_t maximum_file_bytes = static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) + 8U;

    if (new_cursor > maximum_file_bytes)
    {
        return stream_bad_range;
    }

    const std::uint64_t committed_payload_end = artifact::pcm24_wav_header_bytes + committed_frames_ * 3U;

    if (writer_cursor_ < committed_payload_end && new_cursor > artifact::pcm24_wav_header_bytes)
    {
        return stream_bad_range;
    }

    UINT actual{};
    const UINT requested = static_cast<UINT>(bytes.size());

    if (f_write(file_, bytes.data(), requested, &actual) != FR_OK)
    {
        return latch(stream_io_error);
    }

    if (actual != requested)
    {
        return latch(stream_short_io);
    }

    writer_cursor_ = new_cursor;

    if (written_extent_ < new_cursor)
    {
        written_extent_ = new_cursor;
    }

    return m110::Status::success();
}

m110::Status Rt1170PreparedStream::seek(const std::uint64_t absolute_offset) noexcept
{
    if (file_ == nullptr)
    {
        return stream_not_bound;
    }

    if (!failure_.is_ok())
    {
        return failure_;
    }

    if (absolute_offset > written_extent_ || absolute_offset > std::numeric_limits<FSIZE_t>::max())
    {
        return stream_bad_range;
    }

    if (f_lseek(file_, static_cast<FSIZE_t>(absolute_offset)) != FR_OK)
    {
        return latch(stream_io_error);
    }

    writer_cursor_ = absolute_offset;
    return m110::Status::success();
}

m110::Status Rt1170PreparedStream::commit_frames(const std::uint64_t frames) noexcept
{
    if (file_ == nullptr)
    {
        return stream_not_bound;
    }

    if (!failure_.is_ok())
    {
        return failure_;
    }

    if (frames < committed_frames_ || frames > (std::numeric_limits<std::uint64_t>::max() - artifact::pcm24_wav_header_bytes) / 3U)
    {
        return stream_bad_range;
    }

    const std::uint64_t data_bytes = frames * 3U;
    const std::uint64_t payload_end = artifact::pcm24_wav_header_bytes + data_bytes;

    if (!valid_file_end(data_bytes) || payload_end > written_extent_ || payload_end > std::numeric_limits<FSIZE_t>::max())
    {
        return stream_bad_range;
    }

    if (f_size(file_) < static_cast<FSIZE_t>(payload_end))
    {
        return latch(stream_io_error);
    }

    if (f_sync(file_) != FR_OK)
    {
        return latch(stream_io_error);
    }

    committed_frames_ = frames;
    return m110::Status::success();
}

m110::Status Rt1170PreparedStream::read_frames(const std::uint64_t first_frame, const std::span<std::uint8_t> output) noexcept
{
    if (file_ == nullptr)
    {
        return stream_not_bound;
    }

    if (!failure_.is_ok())
    {
        return failure_;
    }

    if (output.empty() || output.size() % 3U != 0U || output.size() > std::numeric_limits<UINT>::max())
    {
        return stream_bad_range;
    }

    const auto frames = static_cast<std::uint64_t>(output.size() / 3U);

    if (first_frame > committed_frames_ || frames > committed_frames_ - first_frame || first_frame > (std::numeric_limits<std::uint64_t>::max() - artifact::pcm24_wav_header_bytes) / 3U)
    {
        return stream_bad_range;
    }

    const std::uint64_t offset = artifact::pcm24_wav_header_bytes + first_frame * 3U;

    if (offset > std::numeric_limits<std::uint64_t>::max() - output.size() || offset + output.size() > written_extent_ || offset > std::numeric_limits<FSIZE_t>::max())
    {
        return stream_bad_range;
    }

    const auto alignment_prefix = static_cast<std::size_t>(offset % 4U);

    if (output.size() > read_scratch_.size() || alignment_prefix > read_scratch_.size() - output.size())
    {
        return {.code = m110::StatusCode::buffer_too_small, .message = "prepared stream read exceeds aligned scratch capacity"};
    }

    const std::uint64_t saved_cursor = writer_cursor_;
    m110::Status result = m110::Status::success();

    if (f_lseek(file_, static_cast<FSIZE_t>(offset)) != FR_OK)
    {
        result = stream_io_error;
    }
    else
    {
        auto* const aligned = read_scratch_.data() + alignment_prefix;
        UINT actual{};
        const UINT requested = static_cast<UINT>(output.size());

        if (f_read(file_, aligned, requested, &actual) != FR_OK)
        {
            result = stream_io_error;
        }
        else if (actual != requested)
        {
            result = stream_short_io;
        }
        else
        {
            std::memmove(output.data(), aligned, output.size());
        }
    }

    if (f_lseek(file_, static_cast<FSIZE_t>(saved_cursor)) != FR_OK)
    {
        result = latch(result.is_ok() ? stream_io_error : result);
        return result;
    }

    if (!result.is_ok())
    {
        return latch(result);
    }

    return m110::Status::success();
}

void Rt1170PreparedStream::unbind() noexcept
{
    file_ = nullptr;
    read_scratch_ = {};
}

m110::Status Rt1170PreparedStream::latch(const m110::Status failure) noexcept
{
    if (failure_.is_ok())
    {
        failure_ = failure.is_ok() ? stream_io_error : failure;
    }

    return failure_;
}

bool Rt1170PreparedStream::valid_file_end(const std::uint64_t data_bytes) const noexcept
{
    const std::uint64_t padded_data = data_bytes + (data_bytes & 1U);
    const std::uint64_t riff_size = artifact::pcm24_wav_riff_overhead_bytes + padded_data;
    const std::uint64_t file_size = artifact::pcm24_wav_header_bytes + data_bytes + (data_bytes & 1U);
    return riff_size <= std::numeric_limits<std::uint32_t>::max() && file_size <= std::numeric_limits<FSIZE_t>::max();
}

} // namespace waveform_generator
