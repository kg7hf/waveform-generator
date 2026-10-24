// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

#pragma once

// Bounded offline WAV I/O for signal_lab_render. The engine still sees the
// original block partition. Output is spooled to a temporary file and copied
// to its requested name only after the entire render has been accepted.

#include "common/pcm24.hpp"
#include "common/pcm24_artifact.hpp"
#include "common/wav.hpp"
#include "signal_lab/det_math.hpp"
#include "signal_lab/mixer.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>

#if !defined(_WIN32)
#include <sys/types.h>
#endif

namespace waveform_generator::render
{

static_assert(artifact::pcm24_artifact_max_append_frames >= signal_lab::engine_capacity_frames);

struct TemporaryFileCloser
{
    void operator()(std::FILE* file) const noexcept
    {
        if (file != nullptr)
        {
            static_cast<void>(std::fclose(file));
        }
    }
};

inline constexpr float pcm16_scale = 32768.0F;

inline constexpr m110::Status source_read_failed{.code = m110::StatusCode::io_error, .message = "cannot read source"};
inline constexpr m110::Status output_write_failed{.code = m110::StatusCode::io_error, .message = "cannot write output"};
inline constexpr m110::Status invalid_source{.code = m110::StatusCode::invalid_argument, .message = "source must be a 48 kHz mono PCM24 or legacy PCM16 WAV with one fmt and one data chunk"};

class WavSource
{
public:
    WavSource() = default;
    WavSource(const WavSource&) = delete;
    WavSource& operator=(const WavSource&) = delete;
    WavSource(WavSource&&) = delete;
    WavSource& operator=(WavSource&&) = delete;
    ~WavSource() = default;

    // Replaces any previous source and validates the player's RIFF contract
    // without loading the file. Failure leaves this source closed and empty.
    [[nodiscard]] m110::Status open(const std::string& path)
    {
        close();
        mFile.open(path, std::ios::binary | std::ios::ate);
        const auto size = mFile.tellg();

        if (!mFile || size < 0)
        {
            close();
            return source_read_failed;
        }

        if (size > std::numeric_limits<std::uint32_t>::max())
        {
            close();
            return invalid_source;
        }

        mReader = WavReader{this, static_cast<std::uint32_t>(size), &read_bytes};

        if (validate_pcm24_mono_48k_wav(mReader, mPayload) != WavValidationError::none)
        {
            mPayload = {};

            if (validate_pcm16_mono_48k_wav(mReader, mPayload) != WavValidationError::none)
            {
                close();
                return invalid_source;
            }
        }

        return m110::Status::success();
    }

    // An unopened, closed or rejected source has no frames.
    [[nodiscard]] std::uint64_t frames() const noexcept { return mPayload.bytes_per_frame == 0U ? 0U : mPayload.bytes / mPayload.bytes_per_frame; }

    // Reads an exact, bounded range; a truncated/changed source fails the render.
    [[nodiscard]] m110::Status read(std::uint64_t first, std::span<float> output)
    {
        if (mReader.read == nullptr)
        {
            return wav_read_unready;
        }

        if (output.size() > signal_lab::engine_block_frames || first > frames() || output.size() > frames() - first)
        {
            return wav_read_past_end;
        }

        const auto offset = static_cast<std::uint32_t>(mPayload.offset + first * mPayload.bytes_per_frame);
        const auto count = output.size() * mPayload.bytes_per_frame;

        if (const m110::Status read = wav_read_exact(mReader, offset, std::span{mBytes}.first(count)); !read.is_ok())
        {
            return read;
        }

        for (std::size_t index = 0U; index < output.size(); ++index)
        {
            const auto* at = mBytes.data() + index * mPayload.bytes_per_frame;
            output[index] = mPayload.bytes_per_frame == audio::pcm24_bytes_per_sample ? audio::pcm24_to_float(audio::read_pcm24_le(at))
                                                                                      : static_cast<float>(std::bit_cast<std::int16_t>(wav_read_le16(at))) / pcm16_scale;
        }

        return m110::Status::success();
    }

    // A pre-pass preserves the old summation order and trims only leading and
    // trailing exact zeroes. Interior silence remains in the RMS denominator.
    [[nodiscard]] m110::Result<double> reference(const signal_lab::Scenario& scenario)
    {
        if (mReader.read == nullptr)
        {
            return wav_read_unready;
        }

        std::uint64_t begin = 0U;
        std::uint64_t end = frames();

        if (!scenario.reference_auto)
        {
            begin = signal_lab::det::seconds_to_frames(scenario.reference_start_seconds, signal_lab::sample_rate_hz);
            const auto length = signal_lab::det::seconds_to_frames(scenario.reference_duration_seconds, signal_lab::sample_rate_hz);

            if (begin > end || length > end - begin)
            {
                return m110::Status{.code = m110::StatusCode::invalid_argument, .message = "reference interval extends beyond the source"};
            }

            end = begin + length;
        }

        std::uint64_t first_nonzero = end;
        std::uint64_t after_nonzero = begin;
        double energy = 0.0;
        std::array<float, signal_lab::engine_block_frames> block{};

        for (std::uint64_t cursor = begin; cursor < end;)
        {
            const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(block.size(), end - cursor));
            const auto samples = std::span{block}.first(count);

            if (const m110::Status read = this->read(cursor, samples); !read.is_ok())
            {
                return read;
            }

            for (std::size_t index = 0U; index < count; ++index)
            {
                const double x = samples[index];
                energy += x * x;

                if (x != 0.0)
                {
                    first_nonzero = std::min(first_nonzero, cursor + index);
                    after_nonzero = cursor + index + 1U;
                }
            }

            cursor += count;
        }

        if (scenario.reference_auto)
        {
            begin = first_nonzero;
            end = after_nonzero;
        }

        if (end <= begin)
        {
            return m110::Status{.code = m110::StatusCode::invalid_argument, .message = "source is silent; cannot measure a reference level"};
        }

        return std::sqrt(energy / static_cast<double>(end - begin));
    }

    // Release before publishing, permitting an in-place render on Windows.
    // Idempotent: invalidate metadata and the callback before releasing the file.
    void close()
    {
        mReader = {};
        mPayload = {};

        if (mFile.is_open())
        {
            mFile.close();
        }

        mFile.clear();
    }

private:
    [[nodiscard]] static m110::Status read_bytes(void* context, std::uint32_t offset, std::uint8_t* destination, std::uint32_t count) noexcept
    {
        auto& source = *static_cast<WavSource*>(context);
        auto& file = source.mFile;
        file.clear();
        file.seekg(offset, std::ios::beg);
        auto remaining = std::span{destination, count};

        while (!remaining.empty() && file)
        {
            const auto chunk = std::span{source.mRaw}.first(std::min(remaining.size(), source.mRaw.size()));
            file.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));

            if (file)
            {
                std::ranges::transform(chunk, remaining.begin(), [](char byte) { return std::bit_cast<std::uint8_t>(byte); });

                remaining = remaining.subspan(chunk.size());
            }
        }

        return file ? m110::Status::success() : source_read_failed;
    }

    std::ifstream mFile{};
    WavReader mReader{};
    WavPayload mPayload{};
    std::array<std::uint8_t, signal_lab::engine_block_frames * audio::pcm24_bytes_per_sample> mBytes{};
    std::array<char, signal_lab::engine_block_frames * audio::pcm24_bytes_per_sample> mRaw{};
};

class WavSpool
{
public:
    // Owns the temporary stream; fclose removes it on every return path.
    [[nodiscard]] m110::Status open()
    {
        if (mWriter.state() == artifact::Pcm24ArtifactState::active)
        {
            return m110::Status{.code = m110::StatusCode::busy, .message = "the WAV spool is already active"};
        }

        // tmpfile transfers ownership to the unique_ptr and its fclose deleter.
        mFile = std::unique_ptr<std::FILE, TemporaryFileCloser>{std::tmpfile()};

        if (!mFile)
        {
            return output_write_failed;
        }

        mSink.bind(mFile.get());
        return mWriter.begin(mSink, {.sample_rate_hz = signal_lab::sample_rate_hz, .channels = 1U});
    }

    [[nodiscard]] std::uint64_t frames() const noexcept { return mWriter.frames(); }

    [[nodiscard]] m110::Status append(std::span<const float> samples)
    {
        if (samples.size() > signal_lab::engine_capacity_frames)
        {
            mWriter.cancel();
            return m110::Status{.code = m110::StatusCode::invalid_argument, .message = "output block exceeds the bounded spool capacity"};
        }

        for (std::size_t index = 0U; index < samples.size(); ++index)
        {
            audio::write_pcm24_le(mBytes.data() + index * audio::pcm24_bytes_per_sample, signal_lab::det::quantize_pcm24(samples[index]));
        }

        return mWriter.append_frames(std::span{mBytes}.first(samples.size() * audio::pcm24_bytes_per_sample));
    }

    // Called only after engine and replay acceptance. Processing refusals never
    // open/truncate the user's output; failures publishing it remain I/O failures.
    [[nodiscard]] m110::Status publish(const std::string& path)
    {
        artifact::Pcm24ArtifactSummary summary{};

        if (const auto finished = mWriter.finish(summary); !finished.is_ok())
        {
            return finished;
        }

        if (std::fflush(mFile.get()) != 0 || !mSink.seek(0U).is_ok())
        {
            return output_write_failed;
        }

        std::ofstream output{path, std::ios::binary | std::ios::trunc};
        auto remaining = static_cast<std::uint64_t>(summary.file_bytes);
        std::array<char, signal_lab::engine_capacity_frames * audio::pcm24_bytes_per_sample> read_buffer{};

        while (remaining != 0U && output)
        {
            const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(remaining, read_buffer.size()));

            if (std::fread(read_buffer.data(), 1U, count, mFile.get()) != count)
            {
                return source_read_failed;
            }

            output.write(read_buffer.data(), static_cast<std::streamsize>(count));
            remaining -= count;
        }

        output.close();
        return output.fail() ? output_write_failed : m110::Status::success();
    }

private:
    class FileSink final : public artifact::Pcm24ByteSink
    {
    public:
        void bind(std::FILE* file) noexcept { mFile = file; }

        [[nodiscard]] m110::Status write_exact(std::span<const std::uint8_t> bytes) noexcept override
        {
            if (mFile == nullptr || (!bytes.empty() && std::fwrite(bytes.data(), 1U, bytes.size(), mFile) != bytes.size()))
            {
                return output_write_failed;
            }

            return m110::Status::success();
        }

        [[nodiscard]] m110::Status seek(std::uint64_t offset) noexcept override
        {
            if (mFile == nullptr)
            {
                return output_write_failed;
            }

#if defined(_WIN32)

            if (offset > static_cast<std::uint64_t>(std::numeric_limits<__int64>::max()) || _fseeki64(mFile, static_cast<__int64>(offset), SEEK_SET) != 0)
#else
            if (offset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()) || fseeko(mFile, static_cast<off_t>(offset), SEEK_SET) != 0)
#endif
            {
                return output_write_failed;
            }

            return m110::Status::success();
        }

    private:
        std::FILE* mFile{};
    };

    std::unique_ptr<std::FILE, TemporaryFileCloser> mFile{};
    FileSink mSink{};
    artifact::Pcm24ArtifactWriter mWriter{};
    std::array<std::uint8_t, signal_lab::engine_capacity_frames * audio::pcm24_bytes_per_sample> mBytes{};
};

} // namespace waveform_generator::render
