// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

#pragma once

#include "common/status.hpp"
#include "waveform-source/sha256.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace waveform_generator::artifact
{

inline constexpr std::uint32_t pcm24_wav_header_bytes = 44U;
inline constexpr std::uint32_t pcm24_wav_riff_overhead_bytes = 36U;
inline constexpr std::size_t pcm24_artifact_max_append_frames = 16384U;

// Implementations borrow their storage and must either write every requested
// byte or return an error. seek() uses an absolute byte offset from file start.
class Pcm24ByteSink
{
public:
    Pcm24ByteSink() = default;
    Pcm24ByteSink(const Pcm24ByteSink&) = delete;
    Pcm24ByteSink& operator=(const Pcm24ByteSink&) = delete;
    Pcm24ByteSink(Pcm24ByteSink&&) = delete;
    Pcm24ByteSink& operator=(Pcm24ByteSink&&) = delete;
    virtual ~Pcm24ByteSink() = default;

    [[nodiscard]] virtual m110::Status write_exact(std::span<const std::uint8_t> bytes) noexcept = 0;
    [[nodiscard]] virtual m110::Status seek(std::uint64_t absolute_offset) noexcept = 0;
};

struct Pcm24WavFormat
{
    std::uint32_t sample_rate_hz{};
    std::uint16_t channels{};
};

struct Pcm24ArtifactSummary
{
    std::uint64_t frames{};
    std::uint32_t data_bytes{};
    std::uint64_t file_bytes{};
    std::array<char, 65U> pcm_sha256{};
};

enum class Pcm24ArtifactState : std::uint8_t
{
    idle,
    active,
    complete,
    failed,
    cancelled
};

// Incremental canonical RIFF/WAVE PCM24 writer. The sink must be a fresh,
// empty artifact positioned at offset zero when begin() is called. The caller
// must check finish() status before using the summary; failed finalization
// leaves that output object untouched. This class does not allocate,
// synchronize, close, or publish the borrowed sink.
class Pcm24ArtifactWriter
{
public:
    Pcm24ArtifactWriter() = default;
    ~Pcm24ArtifactWriter() = default;
    Pcm24ArtifactWriter(const Pcm24ArtifactWriter&) = delete;
    Pcm24ArtifactWriter& operator=(const Pcm24ArtifactWriter&) = delete;
    Pcm24ArtifactWriter(Pcm24ArtifactWriter&&) = delete;
    Pcm24ArtifactWriter& operator=(Pcm24ArtifactWriter&&) = delete;

    [[nodiscard]] m110::Status begin(Pcm24ByteSink& sink, Pcm24WavFormat format) noexcept;
    [[nodiscard]] m110::Status append_frames(std::span<const std::uint8_t> packed_pcm24) noexcept;
    [[nodiscard]] m110::Status finish(Pcm24ArtifactSummary& summary) noexcept;
    // Invalidates this writer only; the owner must discard/cancel its sink.
    void cancel() noexcept;

    [[nodiscard]] Pcm24ArtifactState state() const noexcept { return state_; }

    // Progress count only; completed artifact metadata is returned by finish().
    [[nodiscard]] std::uint64_t frames() const noexcept { return frames_; }

    [[nodiscard]] std::uint32_t channels() const noexcept { return format_.channels; }

    [[nodiscard]] m110::Status status() const noexcept { return failure_; }

private:
    [[nodiscard]] m110::Status fail(m110::Status status) noexcept;
    [[nodiscard]] std::array<std::uint8_t, pcm24_wav_header_bytes> make_header(std::uint32_t data_bytes) const noexcept;

    Pcm24ByteSink* sink_{};
    Pcm24WavFormat format_{};
    waveform_source::Sha256 digest_{};
    std::uint64_t frames_{};
    std::uint32_t data_bytes_{};
    Pcm24ArtifactState state_{Pcm24ArtifactState::idle};
    m110::Status failure_{m110::Status::success()};
};

} // namespace waveform_generator::artifact
