// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

#pragma once

// Bounded adapter from the host WAV reader to signal_lab::SecondarySource.
// It reports the RMS of the exact selected excerpt while reads remain in
// absolute frames of the source file.

#include "signal_lab/det_math.hpp"
#include "signal_lab/second_source_params.hpp"
#include "signal_lab/secondary_source.hpp"
#include "stream_wav.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <utility>

namespace waveform_generator::render
{

inline constexpr m110::Status invalid_secondary_source{.code = m110::StatusCode::invalid_argument, .message = "second_source excerpt is empty, outside the source, or silent"};

class SecondaryWavSource final : public signal_lab::SecondarySource
{
public:
    SecondaryWavSource() = default;
    ~SecondaryWavSource() override = default;
    SecondaryWavSource(const SecondaryWavSource&) = delete;
    SecondaryWavSource& operator=(const SecondaryWavSource&) = delete;
    SecondaryWavSource(SecondaryWavSource&&) = delete;
    SecondaryWavSource& operator=(SecondaryWavSource&&) = delete;

    [[nodiscard]] m110::Status open(std::string path, const signal_lab::SecondSourceParams& params)
    {
        path_ = std::move(path);

        if (const m110::Status opened = source_.open(path_); !opened.is_ok())
        {
            return opened;
        }

        frames_ = source_.frames();
        excerpt_start_ = params.source_start_frame;

        if (excerpt_start_ >= frames_)
        {
            return invalid_secondary_source;
        }

        const std::uint64_t available = frames_ - excerpt_start_;
        excerpt_frames_ = params.duration_frames == 0U ? available : params.duration_frames;

        if (excerpt_frames_ == 0U || excerpt_frames_ > available)
        {
            return invalid_secondary_source;
        }

        std::array<float, signal_lab::engine_block_frames> block{};
        double energy = 0.0;
        digest_.reset();

        for (std::uint64_t cursor = 0U; cursor < excerpt_frames_;)
        {
            const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(block.size(), excerpt_frames_ - cursor));

            if (const m110::Status read_status = source_.read(excerpt_start_ + cursor, std::span{block}.first(count)); !read_status.is_ok())
            {
                return read_status;
            }

            for (const float sample : std::span{block}.first(count))
            {
                const double value = sample;
                energy += value * value;
            }

            digest_.update(block.data(), count);
            cursor += count;
        }

        reference_rms_ = std::sqrt(energy / static_cast<double>(excerpt_frames_));

        if (!std::isfinite(reference_rms_) || reference_rms_ <= 0.0)
        {
            return invalid_secondary_source;
        }

        return m110::Status::success();
    }

    [[nodiscard]] std::uint64_t frames() const noexcept override { return frames_; }

    [[nodiscard]] double reference_rms() const noexcept override { return reference_rms_; }

    [[nodiscard]] m110::Status read(std::uint64_t first, std::span<float> output) noexcept override { return source_.read(first, output); }

    [[nodiscard]] std::uint64_t excerpt_start_frame() const noexcept { return excerpt_start_; }

    [[nodiscard]] std::uint64_t excerpt_frames() const noexcept { return excerpt_frames_; }

    [[nodiscard]] const std::string& path() const noexcept { return path_; }

    [[nodiscard]] std::uint64_t pcm_digest() const noexcept { return digest_.value(); }

    void close() { source_.close(); }

private:
    WavSource source_{};
    std::string path_{};
    std::uint64_t frames_{};
    std::uint64_t excerpt_start_{};
    std::uint64_t excerpt_frames_{};
    double reference_rms_{};
    signal_lab::det::StreamDigest digest_{};
};

} // namespace waveform_generator::render
