// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

#pragma once

#include "common/finite_playback.hpp"
#include "common/operation_timing.hpp"
#include "common/status.hpp"

#include <cstdint>
#include <span>

namespace waveform_generator
{
class PrepareJob;

// Player-task owner that schedules finite prepared PCM between a private
// growing WAV and a bounded playback FIFO. All methods are single-owner only.
class PreparedPlayback final
{
public:
    PreparedPlayback() noexcept = default;
    ~PreparedPlayback() = default;
    PreparedPlayback(const PreparedPlayback&) = delete;
    PreparedPlayback& operator=(const PreparedPlayback&) = delete;
    PreparedPlayback(PreparedPlayback&&) = delete;
    PreparedPlayback& operator=(PreparedPlayback&&) = delete;

    // The PrepareJob must already be admitted with begin_managed(). total_frames
    // is taken from the job when it reaches rendering; the remaining config
    // fields are supplied from measured transport limits.
    [[nodiscard]] m110::Status begin(PrepareJob& job, finite_playback::Config config) noexcept;
    // Executes at most one bounded job or scheduler action. Read actions return
    // their exact PCM24 frame count; ordinary progress returns zero.
    [[nodiscard]] m110::Result<std::uint32_t> step(std::span<std::uint8_t> read_output, bool output_drained) noexcept;
    [[nodiscard]] bool ready_to_start() const noexcept;
    [[nodiscard]] m110::Status start() noexcept;
    [[nodiscard]] m110::Status consume(std::uint32_t frames) noexcept;
    [[nodiscard]] m110::Status fail(m110::Status reason) noexcept;
    [[nodiscard]] m110::Status cancel() noexcept;
    [[nodiscard]] bool active() const noexcept { return active_; }

    [[nodiscard]] finite_playback::Snapshot snapshot() const noexcept { return configured_ ? scheduler_.snapshot() : finite_playback::Snapshot{}; }

    [[nodiscard]] m110::Status status() const noexcept { return first_status_; }

    [[nodiscard]] operation_timing::Snapshot timing_snapshot() const noexcept { return timing_.snapshot(); }

private:
    [[nodiscard]] m110::Result<std::uint32_t> stop_step() noexcept;
    [[nodiscard]] m110::Result<std::uint32_t> setup_step() noexcept;
    [[nodiscard]] m110::Result<std::uint32_t> schedule_step(std::span<std::uint8_t> read_output, bool output_drained) noexcept;
    [[nodiscard]] m110::Result<std::uint32_t> action_step(std::span<std::uint8_t> read_output, bool output_drained) noexcept;
    [[nodiscard]] m110::Result<std::uint32_t> produce_action_step(finite_playback::Action action) noexcept;
    [[nodiscard]] m110::Result<std::uint32_t> write_action_step(finite_playback::Action action) noexcept;
    [[nodiscard]] m110::Result<std::uint32_t> barrier_action_step() noexcept;
    [[nodiscard]] m110::Result<std::uint32_t> read_action_step(finite_playback::Action action, std::span<std::uint8_t> read_output) noexcept;
    [[nodiscard]] m110::Result<std::uint32_t> finalize_action_step(bool output_drained) noexcept;
    [[nodiscard]] m110::Status complete_pending(m110::Status result, std::uint32_t frames) noexcept;
    [[nodiscard]] m110::Status latch_failure(m110::Status reason) noexcept;
    [[nodiscard]] m110::Status settle_for_stop() noexcept;
    void release_if_finished() noexcept;

    PrepareJob* job_{};
    finite_playback::Scheduler scheduler_{};
    operation_timing::Accumulator timing_{};
    finite_playback::Config config_{};
    finite_playback::Action pending_ticket_{};
    m110::Status first_status_{m110::Status::success()};
    bool active_{};
    bool configured_{};
    bool action_pending_{};
    bool produce_decoded_{};
    bool finish_started_{};
    bool stopping_{};
    bool job_cancel_sent_{};
    bool cleanup_retry_requested_{};
};

} // namespace waveform_generator
