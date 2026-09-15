// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Paul R. Decker

#pragma once

#include "waveform-source/encoder.hpp"
#include "waveform-source/sha256.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace waveform_source
{

inline constexpr const char* artifact_writer_version = "wfg-wav-artifact/0.2.0";

enum class WavEncoding : std::uint8_t
{
    pcm16_legacy,
    pcm24,
};

class ByteSink
{
public:
    virtual ~ByteSink() = default;
    [[nodiscard]] virtual bool write(const std::uint8_t* bytes, std::size_t count) noexcept = 0;
};

enum class JobState : std::uint8_t { idle, running, complete, failed };

struct ArtifactHashes
{
    char payload[65] {};
    char wav[65] {};
    char pcm[65] {};
};

// Incremental artifact creation shared by a host tool and the SD-card task.
// begin writes the known-length RIFF header; each step renders/writes at most
// 2048 frames. Source and I/O adapter lifetimes must cover the whole job.
// A failed/stopped job has an incomplete WAV that its owner must not publish.
class WavGenerator : private ByteSource
{
public:
    WavGenerator() noexcept = default;
    WavGenerator(const WavGenerator&) = delete;
    WavGenerator& operator=(const WavGenerator&) = delete;
    WavGenerator(WavGenerator&&) = delete;
    WavGenerator& operator=(WavGenerator&&) = delete;

    static constexpr std::size_t maximum_step_frames = 2048U;
    [[nodiscard]] m110::Status begin(Encoder& source, std::string_view profile, ByteSource& payload, std::size_t payload_bytes,
                                     ByteSink& sink, std::uint32_t trailing_silence_frames = 48000U,
                                     WavEncoding encoding = WavEncoding::pcm24) noexcept;
    [[nodiscard]] JobState step(std::size_t max_frames = maximum_step_frames) noexcept;
    void stop() noexcept;
    [[nodiscard]] JobState state() const noexcept
    {
        return state_;
    }
    [[nodiscard]] m110::Status status() const noexcept
    {
        return status_;
    }
    [[nodiscard]] std::uint64_t frames_written() const noexcept
    {
        return frames_written_;
    }
    [[nodiscard]] std::uint64_t total_frames() const noexcept
    {
        return total_frames_;
    }
    // Final artifact digests are available only after state()==complete.
    [[nodiscard]] ArtifactHashes hashes() const noexcept;

private:
    [[nodiscard]] std::size_t read(std::uint8_t* bytes, std::size_t capacity) noexcept override;
    [[nodiscard]] JobState fail(m110::Status status) noexcept;
    Encoder* source_{};
    ByteSource* payload_{};
    ByteSink* sink_{};
    std::size_t payload_bytes_{};
    std::size_t payload_read_{};
    std::uint64_t frames_written_{};
    std::uint64_t total_frames_{};
    std::uint64_t waveform_frames_{};
    Sha256 payload_digest_{};
    Sha256 wav_digest_{};
    Sha256 pcm_digest_{};
    std::array<float, maximum_step_frames> frames_{};
    std::array<std::uint8_t, 3U * maximum_step_frames> bytes_{};
    std::uint32_t sample_bytes_{3U};
    m110::Status status_{};
    JobState state_{JobState::idle};
};

} // namespace waveform_source
