// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

#pragma once

#include "common/status.hpp"

#include <cstdint>

namespace waveform_generator::finite_playback
{

inline constexpr std::uint32_t playback_sample_rate_hz = 48000U;

enum class State : std::uint8_t
{
    unconfigured,
    ready,
    running,
    cancelling,
    completed,
    failed,
    cancelled
};

enum class ActionKind : std::uint8_t
{
    produce,
    write,
    visibility_barrier,
    read,
    finalize
};

struct Config
{
    std::uint64_t total_frames{};
    // A conservative, externally measured end-to-end integer rate. There is
    // deliberately no default: an isolated DSP rate is not a safe overlap rate.
    std::uint32_t measured_rate_frames_per_second{};
    std::uint64_t margin_frames{};
    std::uint32_t queue_capacity_frames{};
    std::uint32_t max_block_frames{};
};

struct Action
{
    std::uint64_t token{};
    std::uint64_t first_frame{};
    std::uint32_t frames{};
    ActionKind kind{ActionKind::produce};
};

struct Snapshot
{
    State state{State::unconfigured};
    std::uint64_t total_frames{};
    std::uint64_t required_lead_frames{};
    std::uint64_t produced_frames{};
    std::uint64_t written_frames{};
    std::uint64_t committed_frames{};
    std::uint64_t read_frames{};
    std::uint64_t consumed_frames{};
    bool started{};
    bool artifact_finalized{};
    bool action_pending{};
    bool owner_released{};
    Action pending_action{};
    m110::Status first_status{m110::Status::success()};
};

// Enforces finite output ordering and watermarks; it does not schedule tasks,
// own buffers/files, or perform I/O. The coordinator executes each bounded
// Action and completes its exact ticket. consume() may advance the audio clock
// while a producer or storage action is pending. A successful write is not
// readable until its visibility_barrier succeeds; a read may never pass that
// committed watermark. External owners must settle an outstanding ticket
// before releasing resources or reconfiguring this object.
class Scheduler
{
public:
    Scheduler() = default;
    ~Scheduler() = default;
    Scheduler(const Scheduler&) = delete;
    Scheduler& operator=(const Scheduler&) = delete;
    Scheduler(Scheduler&&) = delete;
    Scheduler& operator=(Scheduler&&) = delete;

    [[nodiscard]] m110::Status configure(Config config) noexcept;
    [[nodiscard]] m110::Result<Action> begin_produce(std::uint32_t frames) noexcept;
    [[nodiscard]] m110::Result<Action> begin_write(std::uint32_t frames) noexcept;
    [[nodiscard]] m110::Result<Action> begin_visibility_barrier() noexcept;
    [[nodiscard]] m110::Result<Action> begin_read(std::uint32_t frames) noexcept;
    [[nodiscard]] m110::Result<Action> begin_finalize() noexcept;

    // Selects one bounded I/O/producer action without starting playback. Before
    // start it builds the committed lead and queue prefill; while running it
    // refills readable committed data, then flushes staging, then produces.
    // Busy means wait for the owner/audio clock or call start when ready.
    [[nodiscard]] m110::Result<Action> next_action() noexcept;

    // frame_count is the exact count reported by producer/write/read actions;
    // barriers and finalization report zero. A failed Status terminates the
    // stream and remains the first_status. Mismatched tickets are non-mutating.
    [[nodiscard]] m110::Status complete(Action action, m110::Status operation_status, std::uint32_t frame_count = 0U) noexcept;

    // Refuses before start until both the measured lead is committed and the
    // playback queue is prefilled. After start, a short queue is an underrun.
    [[nodiscard]] m110::Status start() noexcept;
    [[nodiscard]] m110::Status consume(std::uint32_t frames) noexcept;
    [[nodiscard]] m110::Status fail(m110::Status reason) noexcept;
    // A pending action remains outstanding until its owner acknowledges it
    // with complete(); cancellation does not imply external I/O was closed.
    [[nodiscard]] m110::Status cancel() noexcept;
    // The external owner calls this only after transport shutdown and cleanup.
    // It is the explicit boundary that permits a terminal run to be reconfigured.
    [[nodiscard]] m110::Status release() noexcept;
    [[nodiscard]] Snapshot snapshot() const noexcept;

private:
    [[nodiscard]] m110::Result<Action> begin(ActionKind kind, std::uint64_t first_frame, std::uint32_t frames) noexcept;
    [[nodiscard]] bool action_matches(Action action) const noexcept;
    [[nodiscard]] bool is_terminal() const noexcept;
    [[nodiscard]] m110::Status latch(m110::Status reason, State state) noexcept;
    void reset_run(Config config, std::uint64_t required_lead_frames) noexcept;

    Config config_{};
    std::uint64_t required_lead_frames_{};
    std::uint64_t produced_frames_{};
    std::uint64_t written_frames_{};
    std::uint64_t committed_frames_{};
    std::uint64_t read_frames_{};
    std::uint64_t consumed_frames_{};
    std::uint64_t next_token_{1U};
    Action pending_action_{};
    m110::Status first_status_{m110::Status::success()};
    State state_{State::unconfigured};
    bool action_pending_{};
    bool started_{};
    bool artifact_finalized_{};
    bool owner_released_{true};
};

} // namespace waveform_generator::finite_playback
