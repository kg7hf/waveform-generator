// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

#include "common/finite_playback.hpp"

#include <algorithm>
#include <limits>

namespace waveform_generator::finite_playback
{
namespace
{

constexpr m110::Status invalid_config{.code = m110::StatusCode::invalid_configuration, .message = "invalid finite playback configuration"};

[[nodiscard]] constexpr m110::Status invalid_argument(const char* message) noexcept
{
    return {.code = m110::StatusCode::invalid_argument, .message = message};
}

[[nodiscard]] constexpr m110::Status busy_status(const char* message) noexcept
{
    return {.code = m110::StatusCode::busy, .message = message};
}

[[nodiscard]] constexpr m110::Status io_error(const char* message) noexcept
{
    return {.code = m110::StatusCode::io_error, .message = message};
}

[[nodiscard]] constexpr m110::Status unavailable(const char* message) noexcept
{
    return {.code = m110::StatusCode::unavailable, .message = message};
}

[[nodiscard]] constexpr std::uint64_t required_lead(const Config config) noexcept
{
    const auto rate = static_cast<std::uint64_t>(config.measured_rate_frames_per_second);
    const auto deficit = rate < playback_sample_rate_hz ? playback_sample_rate_hz - rate : 0U;
    const auto whole_seconds = config.total_frames / playback_sample_rate_hz;
    const auto remainder = config.total_frames % playback_sample_rate_hz;
    const auto fractional_numerator = remainder * deficit;
    const auto fractional_frames = (fractional_numerator + playback_sample_rate_hz - 1U) / playback_sample_rate_hz;
    const auto base = whole_seconds * deficit + fractional_frames;
    const auto lead_without_margin = std::min(base, config.total_frames);
    const auto remaining = config.total_frames - lead_without_margin;
    return config.margin_frames >= remaining ? config.total_frames : lead_without_margin + config.margin_frames;
}

} // namespace

m110::Status Scheduler::configure(const Config config) noexcept
{
    const auto released_terminal = is_terminal() && owner_released_;

    if (action_pending_ || (state_ != State::unconfigured && !released_terminal))
    {
        return busy_status("finite playback is still active");
    }

    if (config.total_frames == 0U || config.measured_rate_frames_per_second == 0U || config.queue_capacity_frames == 0U || config.max_block_frames == 0U ||
        config.max_block_frames > config.queue_capacity_frames)
    {
        return invalid_config;
    }

    reset_run(config, required_lead(config));
    state_ = State::ready;
    return m110::Status::success();
}

m110::Result<Action> Scheduler::begin_produce(const std::uint32_t frames) noexcept
{
    if (state_ != State::ready && state_ != State::running)
    {
        return invalid_config;
    }

    if (frames == 0U || frames > config_.max_block_frames || frames > config_.total_frames - produced_frames_)
    {
        return invalid_argument("invalid finite playback production range");
    }

    const auto staging_used = produced_frames_ - written_frames_;

    if (frames > config_.max_block_frames - staging_used)
    {
        return busy_status("finite playback producer staging is full");
    }

    return begin(ActionKind::produce, produced_frames_, frames);
}

m110::Result<Action> Scheduler::begin_write(const std::uint32_t frames) noexcept
{
    if (state_ != State::ready && state_ != State::running)
    {
        return invalid_config;
    }

    if (written_frames_ != committed_frames_ || frames == 0U || frames > config_.max_block_frames || frames > produced_frames_ - written_frames_)
    {
        return invalid_argument("invalid finite playback write range");
    }

    return begin(ActionKind::write, written_frames_, frames);
}

m110::Result<Action> Scheduler::begin_visibility_barrier() noexcept
{
    if ((state_ != State::ready && state_ != State::running) || written_frames_ == committed_frames_)
    {
        return invalid_config;
    }

    const auto pending_frames = written_frames_ - committed_frames_;

    if (pending_frames > config_.max_block_frames)
    {
        return io_error("finite playback visibility range exceeds one block");
    }

    return begin(ActionKind::visibility_barrier, committed_frames_, static_cast<std::uint32_t>(pending_frames));
}

m110::Result<Action> Scheduler::begin_read(const std::uint32_t frames) noexcept
{
    if (state_ != State::ready && state_ != State::running)
    {
        return invalid_config;
    }

    if (frames == 0U || frames > config_.max_block_frames || frames > committed_frames_ - read_frames_)
    {
        return invalid_argument("finite playback read exceeds committed data");
    }

    const auto queued_frames = read_frames_ - consumed_frames_;

    if (frames > config_.queue_capacity_frames - queued_frames)
    {
        return busy_status("finite playback queue is full");
    }

    return begin(ActionKind::read, read_frames_, frames);
}

m110::Result<Action> Scheduler::begin_finalize() noexcept
{
    if (state_ != State::running || produced_frames_ != config_.total_frames || written_frames_ != config_.total_frames || committed_frames_ != config_.total_frames ||
        read_frames_ != config_.total_frames || consumed_frames_ != config_.total_frames || artifact_finalized_)
    {
        return invalid_config;
    }

    return begin(ActionKind::finalize, config_.total_frames, 0U);
}

m110::Result<Action> Scheduler::next_action() noexcept
{
    if (action_pending_)
    {
        return busy_status("finite playback already has an outstanding action");
    }

    if (state_ != State::ready && state_ != State::running)
    {
        return invalid_config;
    }

    const auto max_block = static_cast<std::uint64_t>(config_.max_block_frames);

    if (state_ == State::ready)
    {
        const auto prefill = std::min<std::uint64_t>(config_.queue_capacity_frames, config_.total_frames);
        const auto committed_target = std::max(required_lead_frames_, prefill);

        if (committed_frames_ < committed_target)
        {
            if (written_frames_ > committed_frames_)
            {
                return begin_visibility_barrier();
            }

            if (produced_frames_ > written_frames_)
            {
                const auto available = produced_frames_ - written_frames_;
                return begin_write(static_cast<std::uint32_t>(std::min(max_block, available)));
            }

            const auto remaining = config_.total_frames - produced_frames_;
            return begin_produce(static_cast<std::uint32_t>(std::min(max_block, remaining)));
        }

        if (read_frames_ - consumed_frames_ < prefill)
        {
            const auto room = config_.queue_capacity_frames - (read_frames_ - consumed_frames_);
            const auto unread = committed_frames_ - read_frames_;
            const auto count = std::min({max_block, room, unread});

            if (count > 0U)
            {
                return begin_read(static_cast<std::uint32_t>(count));
            }
        }

        return busy_status("finite playback is ready to start");
    }

    const auto room = config_.queue_capacity_frames - (read_frames_ - consumed_frames_);
    const auto committed_unread = committed_frames_ - read_frames_;
    // Wait for one useful read batch rather than spending every owner turn
    // refilling the few frames consumed during the preceding read operation.
    const auto refill_frames = std::min(max_block, committed_unread);

    if (refill_frames > 0U && room >= refill_frames)
    {
        return begin_read(static_cast<std::uint32_t>(refill_frames));
    }

    if (written_frames_ > committed_frames_)
    {
        return begin_visibility_barrier();
    }

    if (produced_frames_ > written_frames_)
    {
        const auto staged = produced_frames_ - written_frames_;
        return begin_write(static_cast<std::uint32_t>(std::min(max_block, staged)));
    }

    if (produced_frames_ < config_.total_frames)
    {
        const auto remaining = config_.total_frames - produced_frames_;
        return begin_produce(static_cast<std::uint32_t>(std::min(max_block, remaining)));
    }

    if (consumed_frames_ == config_.total_frames)
    {
        return begin_finalize();
    }

    return busy_status("finite playback is waiting for queue consumption");
}

m110::Status Scheduler::complete(const Action action, const m110::Status operation_status, const std::uint32_t frame_count) noexcept
{
    if (!action_matches(action))
    {
        return invalid_argument("finite playback action ticket mismatch");
    }

    action_pending_ = false;
    pending_action_ = {};

    if (state_ == State::cancelling || state_ == State::failed)
    {
        if (state_ == State::cancelling)
        {
            state_ = State::cancelled;
        }

        return first_status_;
    }

    if (!operation_status.is_ok())
    {
        return latch(operation_status, State::failed);
    }

    if (action.kind == ActionKind::visibility_barrier || action.kind == ActionKind::finalize)
    {
        if (frame_count != 0U)
        {
            return latch(io_error("finite playback action returned an unexpected frame count"), State::failed);
        }
    }
    else if (frame_count != action.frames)
    {
        return latch(io_error("finite playback action completed a short frame count"), State::failed);
    }

    switch (action.kind)
    {
    case ActionKind::produce:
        if (action.first_frame != produced_frames_)
        {
            return latch(io_error("finite playback producer cursor changed"), State::failed);
        }

        produced_frames_ += action.frames;
        break;

    case ActionKind::write:
        if (action.first_frame != written_frames_ || action.frames > produced_frames_ - written_frames_)
        {
            return latch(io_error("finite playback writer cursor changed"), State::failed);
        }

        written_frames_ += action.frames;
        break;

    case ActionKind::visibility_barrier:
        if (action.first_frame != committed_frames_ || action.first_frame + action.frames != written_frames_)
        {
            return latch(io_error("finite playback visibility watermark changed"), State::failed);
        }

        committed_frames_ = written_frames_;
        break;

    case ActionKind::read:
        if (action.first_frame != read_frames_ || action.first_frame + action.frames > committed_frames_)
        {
            return latch(io_error("finite playback read crossed its committed watermark"), State::failed);
        }

        read_frames_ += action.frames;
        break;

    case ActionKind::finalize:
        artifact_finalized_ = true;
        state_ = State::completed;
        break;
    }

    return m110::Status::success();
}

m110::Status Scheduler::start() noexcept
{
    if (state_ != State::ready)
    {
        return invalid_config;
    }

    const auto required_prefill = std::min<std::uint64_t>(config_.queue_capacity_frames, config_.total_frames);

    if (committed_frames_ < required_lead_frames_ || read_frames_ - consumed_frames_ < required_prefill)
    {
        return busy_status("finite playback lead or queue prefill is not ready");
    }

    started_ = true;
    state_ = State::running;
    return m110::Status::success();
}

m110::Status Scheduler::consume(const std::uint32_t frames) noexcept
{
    if (!started_ || state_ != State::running || frames == 0U || frames > config_.max_block_frames)
    {
        return invalid_argument("invalid finite playback consume request");
    }

    if (frames > read_frames_ - consumed_frames_)
    {
        return latch({.code = m110::StatusCode::timeout, .message = "finite playback queue underrun"}, State::failed);
    }

    consumed_frames_ += frames;
    return m110::Status::success();
}

m110::Status Scheduler::fail(const m110::Status reason) noexcept
{
    if (is_terminal())
    {
        return first_status_;
    }

    if (state_ == State::unconfigured)
    {
        return invalid_config;
    }

    if (reason.is_ok())
    {
        return latch({.code = m110::StatusCode::internal_error, .message = "finite playback failed without an error status"}, State::failed);
    }

    return latch(reason, State::failed);
}

m110::Status Scheduler::cancel() noexcept
{
    if (is_terminal())
    {
        return first_status_;
    }

    if (state_ == State::unconfigured)
    {
        return invalid_config;
    }

    const auto status = latch(unavailable("finite playback cancelled"), action_pending_ ? State::cancelling : State::cancelled);
    return status;
}

m110::Status Scheduler::release() noexcept
{
    if (!is_terminal() || action_pending_)
    {
        return busy_status("finite playback is not terminal and settled");
    }

    owner_released_ = true;
    return m110::Status::success();
}

Snapshot Scheduler::snapshot() const noexcept
{
    return {.state = state_,
            .total_frames = config_.total_frames,
            .required_lead_frames = required_lead_frames_,
            .produced_frames = produced_frames_,
            .written_frames = written_frames_,
            .committed_frames = committed_frames_,
            .read_frames = read_frames_,
            .consumed_frames = consumed_frames_,
            .started = started_,
            .artifact_finalized = artifact_finalized_,
            .action_pending = action_pending_,
            .owner_released = owner_released_,
            .pending_action = pending_action_,
            .first_status = first_status_};
}

m110::Result<Action> Scheduler::begin(const ActionKind kind, const std::uint64_t first_frame, const std::uint32_t frames) noexcept
{
    if (action_pending_)
    {
        return busy_status("finite playback already has an outstanding action");
    }

    if (next_token_ == std::numeric_limits<std::uint64_t>::max())
    {
        return io_error("finite playback action ticket space exhausted");
    }

    pending_action_ = {.token = next_token_++, .first_frame = first_frame, .frames = frames, .kind = kind};
    action_pending_ = true;
    return pending_action_;
}

bool Scheduler::action_matches(const Action action) const noexcept
{
    return action_pending_ && action.token == pending_action_.token && action.first_frame == pending_action_.first_frame && action.frames == pending_action_.frames &&
           action.kind == pending_action_.kind;
}

bool Scheduler::is_terminal() const noexcept
{
    return state_ == State::completed || state_ == State::failed || state_ == State::cancelled;
}

m110::Status Scheduler::latch(const m110::Status reason, const State state) noexcept
{
    if (first_status_.is_ok())
    {
        first_status_ = reason.is_ok() ? m110::Status{.code = m110::StatusCode::internal_error, .message = "finite playback latched an empty failure"} : reason;
    }

    state_ = state;
    return first_status_;
}

void Scheduler::reset_run(const Config config, const std::uint64_t lead) noexcept
{
    config_ = config;
    required_lead_frames_ = lead;
    produced_frames_ = 0U;
    written_frames_ = 0U;
    committed_frames_ = 0U;
    read_frames_ = 0U;
    consumed_frames_ = 0U;
    pending_action_ = {};
    first_status_ = m110::Status::success();
    action_pending_ = false;
    started_ = false;
    artifact_finalized_ = false;
    owner_released_ = false;
}

} // namespace waveform_generator::finite_playback
