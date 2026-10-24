// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

#pragma once

// Fixed-storage FatFs provider for a measured PCM24 mono 48 kHz second source.
// The player task is the sole FatFs owner; call close() explicitly there.

#include "common/pcm24.hpp"
#include "common/status.hpp"
#include "common/wav.hpp"
#include "signal_lab/det_math.hpp"
#include "signal_lab/impairment.hpp"
#include "signal_lab/second_source_params.hpp"
#include "signal_lab/secondary_source.hpp"

extern "C"
{
#include "ff.h"
}

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <string_view>
#include <utility>

namespace waveform_generator
{

enum class SecondaryMeasureProgress : std::uint8_t
{
    in_progress,
    complete,
};

inline constexpr std::size_t secondary_wav_frames_per_read = signal_lab::engine_block_frames;
inline constexpr std::size_t secondary_wav_bytes_per_read = secondary_wav_frames_per_read * audio::pcm24_bytes_per_sample;
inline constexpr std::size_t secondary_wav_alignment_bytes = 4U;
inline constexpr std::size_t secondary_wav_buffer_bytes = secondary_wav_bytes_per_read + secondary_wav_alignment_bytes - 1U;
inline constexpr std::size_t secondary_wav_basename_limit = 255U;

inline constexpr m110::Status secondary_wav_bad_name{.code = m110::StatusCode::invalid_argument, .message = "second_source: invalid SD WAV basename"};
inline constexpr m110::Status secondary_wav_open_failed{.code = m110::StatusCode::io_error, .message = "second_source: SD WAV open failed"};
inline constexpr m110::Status secondary_wav_format_invalid{.code = m110::StatusCode::invalid_configuration, .message = "second_source: SD WAV must be mono PCM24 48 kHz"};
inline constexpr m110::Status secondary_wav_seek_failed{.code = m110::StatusCode::io_error, .message = "second_source: SD WAV seek failed"};
inline constexpr m110::Status secondary_wav_read_failed{.code = m110::StatusCode::io_error, .message = "second_source: SD WAV read failed or was short"};
inline constexpr m110::Status secondary_wav_close_failed{.code = m110::StatusCode::io_error, .message = "second_source: SD WAV close failed"};
inline constexpr m110::Status secondary_wav_not_ready{.code = m110::StatusCode::invalid_configuration, .message = "second_source: SD WAV is not calibrated"};
inline constexpr m110::Status secondary_wav_bad_measure_buffer{.code = m110::StatusCode::invalid_argument, .message = "second_source: invalid measurement buffer"};
inline constexpr m110::Status secondary_wav_bad_range{.code = m110::StatusCode::invalid_argument, .message = "second_source: SD WAV range is outside the selected excerpt"};
inline constexpr m110::Status secondary_wav_empty_or_silent{.code = m110::StatusCode::invalid_configuration, .message = "second_source: SD WAV excerpt is empty or silent"};

class Rt1170SecondaryWav final : public signal_lab::SecondarySource
{
public:
    Rt1170SecondaryWav() noexcept = default;
    ~Rt1170SecondaryWav() override = default;
    Rt1170SecondaryWav(const Rt1170SecondaryWav&) = delete;
    Rt1170SecondaryWav& operator=(const Rt1170SecondaryWav&) = delete;
    Rt1170SecondaryWav(Rt1170SecondaryWav&&) = delete;
    Rt1170SecondaryWav& operator=(Rt1170SecondaryWav&&) = delete;

    [[nodiscard]] m110::Status open(std::string_view basename, const signal_lab::SecondSourceParams& params) noexcept
    {
        if (const m110::Status closed = close(); !closed.is_ok())
        {
            return closed;
        }

        if (!valid_basename(basename))
        {
            return secondary_wav_bad_name;
        }

        std::array<char, media_path_capacity> path{};

        if (const m110::Status path_status = make_path(basename, path); !path_status.is_ok())
        {
            return path_status;
        }

        if (f_open(&mFile, path.data(), FA_READ) != FR_OK)
        {
            return secondary_wav_open_failed;
        }

        mOpen = true;
        const FSIZE_t size = f_size(&mFile);

        if (std::cmp_greater(size, std::numeric_limits<std::uint32_t>::max()))
        {
            return fail_open(secondary_wav_format_invalid);
        }

        mFileBytes = static_cast<std::uint32_t>(size);
        mLastIoFailure = m110::Status::success();
        const WavReader reader{.context = this, .file_size = mFileBytes, .read = &read_wav_callback};
        const WavValidationError wav = validate_pcm24_mono_48k_wav(reader, mPayload);

        if (wav != WavValidationError::none)
        {
            const m110::Status failure = wav == WavValidationError::io && !mLastIoFailure.is_ok() ? mLastIoFailure : secondary_wav_format_invalid;
            return fail_open(failure);
        }

        if (const m110::Status valid = signal_lab::validate_second_source_params(params); !valid.is_ok())
        {
            return fail_open(valid);
        }

        mFrames = mPayload.bytes / audio::pcm24_bytes_per_sample;

        if (const m110::Status excerpt_status = select_excerpt(params); !excerpt_status.is_ok())
        {
            return fail_open(excerpt_status);
        }

        mMeasureDigest.reset();
        mMeasureEnergy = 0.0;
        mMeasureFrames = 0U;
        mReferenceRms = 0.0;
        mMeasureComplete = false;
        mMeasureFailed = false;
        mMeasureFailure = m110::Status::success();
        mReadFailed = false;
        mReadFailure = m110::Status::success();
        return m110::Status::success();
    }

    [[nodiscard]] m110::Result<SecondaryMeasureProgress> measure_step(std::span<float> scratch) noexcept
    {
        if (!mOpen || mMeasureFailed)
        {
            return mMeasureFailed ? mMeasureFailure : secondary_wav_not_ready;
        }

        if (mMeasureComplete)
        {
            return SecondaryMeasureProgress::complete;
        }

        if (scratch.empty() || scratch.size() > secondary_wav_frames_per_read)
        {
            return secondary_wav_bad_measure_buffer;
        }

        const std::uint64_t remaining = mSelectedFrames - mMeasureFrames;
        const std::size_t count = std::min<std::size_t>(scratch.size(), static_cast<std::size_t>(remaining));
        const std::uint64_t first = mSelectedStart + mMeasureFrames;

        if (const m110::Status read = raw_read(first, scratch.first(count)); !read.is_ok())
        {
            mMeasureFailed = true;
            mMeasureFailure = read;
            return read;
        }

        for (const float sample : scratch.first(count))
        {
            if (!std::isfinite(sample))
            {
                mMeasureFailed = true;
                mMeasureFailure = secondary_wav_read_failed;
                return mMeasureFailure;
            }

            mMeasureEnergy += static_cast<double>(sample) * static_cast<double>(sample);
        }

        mMeasureDigest.update(scratch.data(), count);
        mMeasureFrames += count;

        if (mMeasureFrames != mSelectedFrames)
        {
            return SecondaryMeasureProgress::in_progress;
        }

        mReferenceRms = std::sqrt(mMeasureEnergy / static_cast<double>(mMeasureFrames));

        if (!std::isfinite(mReferenceRms) || !(mReferenceRms > 0.0))
        {
            mMeasureFailed = true;
            mMeasureFailure = secondary_wav_empty_or_silent;
            return mMeasureFailure;
        }

        mMeasureComplete = true;
        return SecondaryMeasureProgress::complete;
    }

    [[nodiscard]] m110::Status read(std::uint64_t first, std::span<float> output) noexcept override
    {
        if (!mOpen || !mMeasureComplete || mMeasureFailed)
        {
            return secondary_wav_not_ready;
        }

        if (mReadFailed)
        {
            return mReadFailure;
        }

        if (output.size() > secondary_wav_frames_per_read || first < mSelectedStart || first > selected_end() || output.size() > selected_end() - first)
        {
            return secondary_wav_bad_range;
        }

        if (output.empty())
        {
            return m110::Status::success();
        }

        if (const m110::Status status = raw_read(first, output); !status.is_ok())
        {
            mReadFailed = true;
            mReadFailure = status;
            return status;
        }

        return m110::Status::success();
    }

    [[nodiscard]] m110::Status close() noexcept
    {
        if (mOpen && f_close(&mFile) != FR_OK)
        {
            // FatFs has not released this handle. Keep it available for a
            // later owner cleanup attempt; the card must remain Local.
            return secondary_wav_close_failed;
        }

        reset_metadata();
        return m110::Status::success();
    }

    [[nodiscard]] std::uint64_t frames() const noexcept override { return mFrames; }

    [[nodiscard]] double reference_rms() const noexcept override { return mMeasureComplete ? mReferenceRms : 0.0; }

    [[nodiscard]] std::uint64_t measured_digest() const noexcept { return mMeasureComplete ? mMeasureDigest.value() : 0U; }

    [[nodiscard]] std::uint64_t selected_start() const noexcept { return mSelectedStart; }

    [[nodiscard]] std::uint64_t selected_frames() const noexcept { return mSelectedFrames; }

private:
    static constexpr std::string_view media_directory{"2:/WG/"};
    static constexpr std::size_t media_path_capacity = media_directory.size() + secondary_wav_basename_limit + 1U;

    [[nodiscard]] static bool valid_basename(std::string_view basename) noexcept
    {
        if (basename.empty() || basename.size() > secondary_wav_basename_limit || basename.size() < 4U || basename.find("..") != std::string_view::npos || basename.back() == '.' ||
            basename.back() == ' ')
        {
            return false;
        }

        for (const char value : basename)
        {
            const auto byte = static_cast<unsigned char>(value);

            if (byte < 0x20U || byte > 0x7EU || value == '/' || value == '\\' || value == ':' || value == '*' || value == '?' || value == '<' || value == '>' || value == '|' || value == '"')
            {
                return false;
            }
        }

        return is_wav_extension(basename);
    }

    [[nodiscard]] static bool is_wav_extension(std::string_view basename) noexcept
    {
        const std::size_t extension = basename.size() - 4U;
        return basename[extension] == '.' && to_ascii_lower(basename[extension + 1U]) == 'w' && to_ascii_lower(basename[extension + 2U]) == 'a' && to_ascii_lower(basename[extension + 3U]) == 'v';
    }

    [[nodiscard]] static char to_ascii_lower(char value) noexcept { return value >= 'A' && value <= 'Z' ? static_cast<char>(value + ('a' - 'A')) : value; }

    [[nodiscard]] static m110::Status make_path(std::string_view basename, std::span<char, media_path_capacity> path) noexcept
    {
        if (media_directory.size() + basename.size() >= path.size())
        {
            return secondary_wav_bad_name;
        }

        std::copy(media_directory.begin(), media_directory.end(), path.begin());
        std::copy(basename.begin(), basename.end(), path.begin() + static_cast<std::ptrdiff_t>(media_directory.size()));
        path[media_directory.size() + basename.size()] = '\0';
        return m110::Status::success();
    }

    [[nodiscard]] static m110::Status read_wav_callback(void* context, std::uint32_t offset, std::uint8_t* destination, std::uint32_t bytes) noexcept
    {
        auto& self = *static_cast<Rt1170SecondaryWav*>(context);
        const m110::Status status = self.read_file_bytes(offset, std::span<std::uint8_t>{destination, bytes});

        if (!status.is_ok())
        {
            self.mLastIoFailure = status;
        }

        return status;
    }

    [[nodiscard]] m110::Status read_file_bytes(std::uint32_t offset, std::span<std::uint8_t> destination) noexcept
    {
        if (!mOpen || (destination.size() != 0U && destination.data() == nullptr) || offset > mFileBytes || destination.size() > mFileBytes - offset ||
            destination.size() > secondary_wav_bytes_per_read)
        {
            return secondary_wav_read_failed;
        }

        if (destination.empty())
        {
            return m110::Status::success();
        }

        if (f_lseek(&mFile, static_cast<FSIZE_t>(offset)) != FR_OK)
        {
            return secondary_wav_seek_failed;
        }

        auto* const aligned = mRawBuffer.data() + (offset % secondary_wav_alignment_bytes);
        UINT actual{};
        const UINT requested = static_cast<UINT>(destination.size());

        if (f_read(&mFile, aligned, requested, &actual) != FR_OK || actual != requested)
        {
            return secondary_wav_read_failed;
        }

        if (destination.data() != aligned)
        {
            std::memcpy(destination.data(), aligned, destination.size());
        }

        return m110::Status::success();
    }

    [[nodiscard]] m110::Status raw_read(std::uint64_t first, std::span<float> output) noexcept
    {
        if (output.size() > secondary_wav_frames_per_read || first > mFrames || output.size() > mFrames - first)
        {
            return secondary_wav_bad_range;
        }

        if (output.empty())
        {
            return m110::Status::success();
        }

        const std::uint64_t data_offset = mPayload.offset;
        const std::uint64_t maximum = std::numeric_limits<std::uint32_t>::max();

        if (first > (maximum - data_offset) / audio::pcm24_bytes_per_sample)
        {
            return secondary_wav_bad_range;
        }

        const std::uint64_t offset = data_offset + first * audio::pcm24_bytes_per_sample;
        const std::size_t bytes = output.size() * audio::pcm24_bytes_per_sample;

        if (offset > maximum || bytes > secondary_wav_bytes_per_read || offset > data_offset + mPayload.bytes || bytes > data_offset + mPayload.bytes - offset)
        {
            return secondary_wav_bad_range;
        }

        const m110::Status read_status = read_file_bytes(static_cast<std::uint32_t>(offset), std::span<std::uint8_t>{mRawBuffer.data() + (offset % secondary_wav_alignment_bytes), bytes});

        if (!read_status.is_ok())
        {
            return read_status;
        }

        const auto* packed = mRawBuffer.data() + (offset % secondary_wav_alignment_bytes);

        for (std::size_t index = 0U; index < output.size(); ++index)
        {
            output[index] = audio::pcm24_to_float(audio::read_pcm24_le(packed + index * audio::pcm24_bytes_per_sample));
        }

        return m110::Status::success();
    }

    [[nodiscard]] m110::Status select_excerpt(const signal_lab::SecondSourceParams& params) noexcept
    {
        if (params.source_start_frame >= mFrames)
        {
            return secondary_wav_bad_range;
        }

        mSelectedStart = params.source_start_frame;
        const std::uint64_t available = mFrames - mSelectedStart;
        mSelectedFrames = params.duration_frames == 0U ? available : params.duration_frames;

        if (mSelectedFrames == 0U || mSelectedFrames > available || mSelectedFrames > signal_lab::max_second_source_frames)
        {
            return secondary_wav_bad_range;
        }

        return m110::Status::success();
    }

    [[nodiscard]] std::uint64_t selected_end() const noexcept { return mSelectedStart + mSelectedFrames; }

    [[nodiscard]] m110::Status fail_open(m110::Status failure) noexcept
    {
        const m110::Status closed = close();
        return closed.is_ok() ? failure : closed;
    }

    void reset_metadata() noexcept
    {
        mOpen = false;
        mFileBytes = 0U;
        mPayload = WavPayload{};
        mFrames = 0U;
        mSelectedStart = 0U;
        mSelectedFrames = 0U;
        mMeasureFrames = 0U;
        mMeasureEnergy = 0.0;
        mReferenceRms = 0.0;
        mMeasureDigest.reset();
        mMeasureComplete = false;
        mMeasureFailed = false;
        mMeasureFailure = m110::Status::success();
        mReadFailed = false;
        mReadFailure = m110::Status::success();
        mLastIoFailure = m110::Status::success();
    }

    FIL mFile{};
    alignas(secondary_wav_alignment_bytes) std::array<std::uint8_t, secondary_wav_buffer_bytes> mRawBuffer{};
    signal_lab::det::StreamDigest mMeasureDigest{};
    WavPayload mPayload{};
    m110::Status mMeasureFailure{};
    m110::Status mReadFailure{};
    m110::Status mLastIoFailure{};
    std::uint64_t mFrames{};
    std::uint64_t mSelectedStart{};
    std::uint64_t mSelectedFrames{};
    std::uint64_t mMeasureFrames{};
    double mMeasureEnergy{};
    double mReferenceRms{};
    std::uint32_t mFileBytes{};
    bool mOpen{};
    bool mMeasureComplete{};
    bool mMeasureFailed{};
    bool mReadFailed{};
};

} // namespace waveform_generator
