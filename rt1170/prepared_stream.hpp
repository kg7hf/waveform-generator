// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

#pragma once

#include "common/pcm24_artifact.hpp"

extern "C"
{
#include "ff.h"
}

#include <cstddef>
#include <cstdint>
#include <span>

namespace waveform_generator
{

// A borrowed, single-FIL PCM24 stream for interleaving writer appends with
// committed playback reads. The owner opens a new empty file and owns every
// mount/open/close/unlink operation. No data is readable until commit_frames()
// successfully syncs the same FIL, and a read always restores its write cursor.
class Rt1170PreparedStream final : public artifact::Pcm24ByteSink
{
public:
    Rt1170PreparedStream() noexcept = default;
    ~Rt1170PreparedStream() override = default;
    Rt1170PreparedStream(const Rt1170PreparedStream&) = delete;
    Rt1170PreparedStream& operator=(const Rt1170PreparedStream&) = delete;
    Rt1170PreparedStream(Rt1170PreparedStream&&) = delete;
    Rt1170PreparedStream& operator=(Rt1170PreparedStream&&) = delete;

    // The caller supplies a fresh CREATE_NEW read/write FIL positioned at 0.
    // read_scratch is borrowed and must outlive this binding. Binding itself
    // does no FatFs I/O.
    [[nodiscard]] m110::Status bind(FIL* file, std::span<std::uint8_t> read_scratch) noexcept;
    [[nodiscard]] m110::Status write_exact(std::span<const std::uint8_t> bytes) noexcept override;
    [[nodiscard]] m110::Status seek(std::uint64_t absolute_offset) noexcept override;

    // Commits only an extent already written exactly; a failed sync leaves the
    // previous committed frame watermark unchanged.
    [[nodiscard]] m110::Status commit_frames(std::uint64_t frames) noexcept;
    // Reads packed mono PCM24 frames from the committed payload. output must
    // contain a nonempty whole number of frames. Read and cursor-restore errors
    // latch the stream; output is usable only after success.
    [[nodiscard]] m110::Status read_frames(std::uint64_t first_frame, std::span<std::uint8_t> output) noexcept;

    // The owner calls this only after it has closed or otherwise relinquished
    // the FIL. It performs no FatFs operation.
    void unbind() noexcept;

    [[nodiscard]] std::uint64_t committed_frames() const noexcept { return committed_frames_; }

    [[nodiscard]] m110::Status status() const noexcept { return failure_; }

private:
    [[nodiscard]] m110::Status latch(m110::Status failure) noexcept;
    [[nodiscard]] bool valid_file_end(std::uint64_t data_bytes) const noexcept;

    FIL* file_{};
    std::span<std::uint8_t> read_scratch_{};
    std::uint64_t writer_cursor_{};
    std::uint64_t written_extent_{};
    std::uint64_t committed_frames_{};
    m110::Status failure_{m110::Status::success()};
};

} // namespace waveform_generator
