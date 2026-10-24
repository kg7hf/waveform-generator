// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

#include "prepared_playback.hpp"

#include "common/operation_timing.hpp"
#include "os/critical_section.hpp"
#include "platform/board.hpp"
#include "prepare_job.hpp"
#include "signal_lab/mixer.hpp"

extern "C"
{
#include "FreeRTOS.h"
#include "task.h"
}

#include <algorithm>

namespace waveform_generator
{
namespace
{

[[nodiscard]] operation_timing::Sample timing_sample() noexcept
{
    const TaskCriticalSection lock;
    return {.cycles = m110::platform_cycle_counter(), .ticks = xTaskGetTickCount(), .frequency_hz = m110::platform_cycle_counter_frequency_hz()};
}

inline constexpr m110::Status playback_inactive{.code = m110::StatusCode::unavailable, .message = "prepared playback is not active"};
inline constexpr m110::Status playback_config_invalid{.code = m110::StatusCode::invalid_configuration, .message = "prepared playback configuration is invalid"};
inline constexpr m110::Status playback_ticket_error{.code = m110::StatusCode::internal_error, .message = "prepared playback action did not match its job cursor"};
inline constexpr m110::Status playback_cancelled{.code = m110::StatusCode::unavailable, .message = "prepared playback was cancelled"};

} // namespace

m110::Status PreparedPlayback::begin(PrepareJob& job, const finite_playback::Config config) noexcept
{
    if (active_)
    {
        return {.code = m110::StatusCode::busy, .message = "prepared playback is already active"};
    }

    if (!job.busy() || job.snapshot().phase != prepare::Phase::starting || config.measured_rate_frames_per_second == 0U || config.queue_capacity_frames == 0U || config.max_block_frames == 0U ||
        config.max_block_frames > signal_lab::engine_block_frames || config.max_block_frames > config.queue_capacity_frames)
    {
        return playback_config_invalid;
    }

    job_ = &job;
    config_ = config;
    config_.total_frames = 0U;
    pending_ticket_ = {};
    first_status_ = m110::Status::success();
    active_ = true;
    configured_ = false;
    action_pending_ = false;
    produce_decoded_ = false;
    finish_started_ = false;
    stopping_ = false;
    job_cancel_sent_ = false;
    cleanup_retry_requested_ = false;
    timing_.reset(m110::platform_cycle_counter_frequency_hz(), static_cast<std::uint32_t>(configTICK_RATE_HZ));
    return m110::Status::success();
}

m110::Result<std::uint32_t> PreparedPlayback::step(const std::span<std::uint8_t> read_output, const bool output_drained) noexcept
{
    if (!active_)
    {
        return playback_inactive;
    }

    if (stopping_)
    {
        return stop_step();
    }

    if (job_ == nullptr)
    {
        return latch_failure(playback_ticket_error);
    }

    const auto current_job = job_->snapshot();

    if (current_job.phase == prepare::Phase::failed)
    {
        const m110::Status error = current_job.status.is_ok() ? playback_ticket_error : current_job.status;
        return latch_failure(error);
    }

    if (current_job.phase == prepare::Phase::cleanup_failed)
    {
        if (first_status_.is_ok())
        {
            first_status_ = current_job.status.is_ok() ? playback_ticket_error : current_job.status;
        }

        stopping_ = true;

        if (configured_)
        {
            static_cast<void>(scheduler_.fail(first_status_));
        }

        return first_status_;
    }

    if (current_job.phase == prepare::Phase::cancelled)
    {
        static_cast<void>(cancel());
        return std::uint32_t{0U};
    }

    if (current_job.phase == prepare::Phase::complete && configured_ && !action_pending_)
    {
        release_if_finished();

        if (!active_ && !first_status_.is_ok())
        {
            return first_status_;
        }

        return std::uint32_t{0U};
    }

    if (!configured_)
    {
        return setup_step();
    }

    return schedule_step(read_output, output_drained);
}

m110::Result<std::uint32_t> PreparedPlayback::setup_step() noexcept
{
    if (job_ == nullptr)
    {
        return latch_failure(playback_ticket_error);
    }

    const auto job_snapshot = job_->snapshot();

    if (job_snapshot.phase == prepare::Phase::failed || job_snapshot.phase == prepare::Phase::cleanup_failed)
    {
        const m110::Status error = job_snapshot.status.is_ok() ? playback_ticket_error : job_snapshot.status;

        if (job_snapshot.phase == prepare::Phase::cleanup_failed)
        {
            if (first_status_.is_ok())
            {
                first_status_ = error;
            }

            stopping_ = true;
            return first_status_;
        }

        return latch_failure(error);
    }

    if (job_snapshot.phase == prepare::Phase::cancelled)
    {
        static_cast<void>(cancel());
        return std::uint32_t{0U};
    }

    if (job_snapshot.phase == prepare::Phase::complete)
    {
        return latch_failure(playback_ticket_error);
    }

    if (job_snapshot.phase != prepare::Phase::rendering)
    {
        if (!job_->busy())
        {
            const m110::Status error = job_snapshot.status.is_ok() ? playback_ticket_error : job_snapshot.status;
            return latch_failure(error);
        }

        const auto timing_begin = timing_sample();
        const m110::Status advanced = job_->step();
        timing_.record(operation_timing::Phase::setup, timing_begin, timing_sample());

        if (!advanced.is_ok() && advanced.code != m110::StatusCode::busy)
        {
            return latch_failure(advanced);
        }

        return std::uint32_t{0U};
    }

    config_.total_frames = job_snapshot.total_input_frames;
    const m110::Status configured = scheduler_.configure(config_);

    if (!configured.is_ok())
    {
        return latch_failure(configured);
    }

    configured_ = true;
    return std::uint32_t{0U};
}

m110::Result<std::uint32_t> PreparedPlayback::schedule_step(const std::span<std::uint8_t> read_output, const bool output_drained) noexcept
{
    if (action_pending_)
    {
        return action_step(read_output, output_drained);
    }

    const auto playback_snapshot = scheduler_.snapshot();

    if (playback_snapshot.started && playback_snapshot.consumed_frames == playback_snapshot.total_frames && !output_drained)
    {
        return std::uint32_t{0U};
    }

    const auto next = scheduler_.next_action();

    if (!next.has_value())
    {
        if (next.error().code == m110::StatusCode::busy)
        {
            return std::uint32_t{0U};
        }

        return latch_failure(next.error());
    }

    pending_ticket_ = next.value();
    action_pending_ = true;
    produce_decoded_ = false;
    finish_started_ = false;
    return action_step(read_output, output_drained);
}

bool PreparedPlayback::ready_to_start() const noexcept
{
    const auto state = scheduler_.snapshot();
    const std::uint64_t prefill = std::min<std::uint64_t>(state.total_frames, config_.queue_capacity_frames);
    return configured_ && !stopping_ && state.state == finite_playback::State::ready && !state.action_pending && state.committed_frames >= state.required_lead_frames &&
           state.read_frames - state.consumed_frames >= prefill;
}

m110::Status PreparedPlayback::start() noexcept
{
    if (!active_ || !configured_ || stopping_)
    {
        return playback_inactive;
    }

    return scheduler_.start();
}

m110::Status PreparedPlayback::consume(const std::uint32_t frames) noexcept
{
    if (!active_ || !configured_ || stopping_)
    {
        return playback_inactive;
    }

    const m110::Status consumed = scheduler_.consume(frames);

    if (!consumed.is_ok() && consumed.code == m110::StatusCode::timeout)
    {
        return latch_failure(consumed);
    }

    return consumed;
}

m110::Status PreparedPlayback::fail(const m110::Status reason) noexcept
{
    if (!active_)
    {
        return playback_inactive;
    }

    if (reason.is_ok())
    {
        return latch_failure(playback_ticket_error);
    }

    return latch_failure(reason);
}

m110::Status PreparedPlayback::cancel() noexcept
{
    if (!active_)
    {
        return playback_inactive;
    }

    if (first_status_.is_ok())
    {
        first_status_ = playback_cancelled;
    }

    stopping_ = true;

    if (configured_)
    {
        static_cast<void>(scheduler_.cancel());
    }

    if (job_ != nullptr && job_->busy())
    {
        const bool retrying_cleanup = job_->snapshot().phase == prepare::Phase::cleanup_failed;
        const m110::Status cancelled = job_->cancel();

        if (cancelled.is_ok())
        {
            job_cancel_sent_ = true;
            cleanup_retry_requested_ = retrying_cleanup;
        }
    }

    return m110::Status::success();
}

m110::Result<std::uint32_t> PreparedPlayback::stop_step() noexcept
{
    static_cast<void>(settle_for_stop());

    if (action_pending_)
    {
        return first_status_.is_ok() ? m110::Result<std::uint32_t>{playback_ticket_error} : m110::Result<std::uint32_t>{first_status_};
    }

    if (job_ != nullptr && job_->busy())
    {
        const auto before = job_->snapshot();

        if (before.phase == prepare::Phase::cleanup_failed && !cleanup_retry_requested_)
        {
            if (first_status_.is_ok())
            {
                first_status_ = before.status.is_ok() ? playback_ticket_error : before.status;
            }

            return first_status_;
        }

        if (!job_cancel_sent_)
        {
            const m110::Status cancelled = job_->cancel();

            if (cancelled.is_ok())
            {
                job_cancel_sent_ = true;
            }
        }

        cleanup_retry_requested_ = false;
        const auto timing_begin = timing_sample();
        static_cast<void>(job_->step());
        timing_.record(operation_timing::Phase::cleanup, timing_begin, timing_sample());

        if (first_status_.is_ok())
        {
            const auto after = job_->snapshot();

            if (after.phase == prepare::Phase::cleanup_failed || after.phase == prepare::Phase::failed)
            {
                first_status_ = after.status.is_ok() ? playback_ticket_error : after.status;
            }
        }

        if (job_->busy())
        {
            if (first_status_.is_ok())
            {
                return std::uint32_t{0U};
            }

            return first_status_;
        }
    }

    release_if_finished();

    if (first_status_.is_ok())
    {
        return std::uint32_t{0U};
    }

    return first_status_;
}

m110::Result<std::uint32_t> PreparedPlayback::action_step(const std::span<std::uint8_t> read_output, const bool output_drained) noexcept
{
    if (!action_pending_ || job_ == nullptr)
    {
        return latch_failure(playback_ticket_error);
    }

    const auto action = pending_ticket_;

    switch (action.kind)
    {
    case finite_playback::ActionKind::produce:
        return produce_action_step(action);

    case finite_playback::ActionKind::write:
        return write_action_step(action);

    case finite_playback::ActionKind::visibility_barrier:
        return barrier_action_step();

    case finite_playback::ActionKind::read:
        return read_action_step(action, read_output);

    case finite_playback::ActionKind::finalize:
        return finalize_action_step(output_drained);
    }

    return complete_pending(playback_ticket_error, 0U);
}

m110::Result<std::uint32_t> PreparedPlayback::produce_action_step(const finite_playback::Action action) noexcept
{
    if (action.first_frame != job_->managed_produced_frames())
    {
        return complete_pending(playback_ticket_error, 0U);
    }

    const auto phase = produce_decoded_ ? operation_timing::Phase::render : operation_timing::Phase::source_read;
    const auto timing_begin = timing_sample();
    const m110::Status produced = job_->produce_step(action.frames);
    timing_.record(phase, timing_begin, timing_sample());

    if (produced.code == m110::StatusCode::busy && !produce_decoded_)
    {
        produce_decoded_ = true;
        return std::uint32_t{0U};
    }

    if (!produced.is_ok())
    {
        return complete_pending(produced, 0U);
    }

    const m110::Status completed = complete_pending(produced, action.frames);

    if (!completed.is_ok())
    {
        return completed;
    }

    return std::uint32_t{0U};
}

m110::Result<std::uint32_t> PreparedPlayback::write_action_step(const finite_playback::Action action) noexcept
{
    if (action.first_frame != job_->snapshot().output_frames)
    {
        return complete_pending(playback_ticket_error, 0U);
    }

    const auto timing_begin = timing_sample();
    const m110::Status appended = job_->append_produced(action.frames);
    timing_.record(operation_timing::Phase::append, timing_begin, timing_sample());

    if (!appended.is_ok())
    {
        return complete_pending(appended, 0U);
    }

    const m110::Status completed = complete_pending(appended, action.frames);

    if (!completed.is_ok())
    {
        return completed;
    }

    return std::uint32_t{0U};
}

m110::Result<std::uint32_t> PreparedPlayback::barrier_action_step() noexcept
{
    const auto timing_begin = timing_sample();
    const m110::Status committed = job_->commit_prefix();
    timing_.record(operation_timing::Phase::commit, timing_begin, timing_sample());

    if (!committed.is_ok())
    {
        return complete_pending(committed, 0U);
    }

    const m110::Status completed = complete_pending(committed, 0U);

    if (!completed.is_ok())
    {
        return completed;
    }

    return std::uint32_t{0U};
}

m110::Result<std::uint32_t> PreparedPlayback::read_action_step(const finite_playback::Action action, const std::span<std::uint8_t> read_output) noexcept
{
    const std::size_t byte_count = static_cast<std::size_t>(action.frames) * 3U;

    if (read_output.size() < byte_count)
    {
        const m110::Status too_small{.code = m110::StatusCode::buffer_too_small, .message = "prepared playback FIFO read span is too small"};
        return complete_pending(too_small, 0U);
    }

    const auto timing_begin = timing_sample();
    const m110::Status read = job_->read_prefix(action.first_frame, read_output.first(byte_count));
    timing_.record(operation_timing::Phase::read, timing_begin, timing_sample());

    if (!read.is_ok())
    {
        return complete_pending(read, 0U);
    }

    const m110::Status completed = complete_pending(read, action.frames);

    if (!completed.is_ok())
    {
        return completed;
    }

    return action.frames;
}

m110::Result<std::uint32_t> PreparedPlayback::finalize_action_step(const bool output_drained) noexcept
{
    if (!output_drained)
    {
        return std::uint32_t{0U};
    }

    if (!finish_started_)
    {
        const auto timing_begin = timing_sample();
        const m110::Status finished = job_->finish_managed();
        timing_.record(operation_timing::Phase::finalize, timing_begin, timing_sample());

        if (!finished.is_ok())
        {
            return complete_pending(finished, 0U);
        }

        finish_started_ = true;
    }

    if (job_->busy())
    {
        const auto timing_begin = timing_sample();
        const m110::Status advanced = job_->step();
        timing_.record(operation_timing::Phase::finalize, timing_begin, timing_sample());

        if (!advanced.is_ok() && advanced.code != m110::StatusCode::busy)
        {
            return complete_pending(advanced, 0U);
        }

        const auto state = job_->snapshot();

        if (state.phase == prepare::Phase::failed || state.phase == prepare::Phase::cleanup_failed)
        {
            const m110::Status error = state.status.is_ok() ? playback_ticket_error : state.status;
            return complete_pending(error, 0U);
        }

        return std::uint32_t{0U};
    }

    const auto state = job_->snapshot();

    if (state.phase != prepare::Phase::complete)
    {
        const m110::Status error = state.status.is_ok() ? playback_ticket_error : state.status;
        return complete_pending(error, 0U);
    }

    const m110::Status completed = complete_pending(m110::Status::success(), 0U);

    if (completed.is_ok())
    {
        release_if_finished();
    }

    if (!completed.is_ok())
    {
        return completed;
    }

    return std::uint32_t{0U};
}

m110::Status PreparedPlayback::complete_pending(const m110::Status result, const std::uint32_t frames) noexcept
{
    if (!action_pending_)
    {
        return latch_failure(playback_ticket_error);
    }

    const finite_playback::Action action = pending_ticket_;
    const m110::Status completed = scheduler_.complete(action, result, frames);

    if (scheduler_.snapshot().action_pending)
    {
        const m110::Status error = completed.is_ok() ? playback_ticket_error : completed;
        return latch_failure(error);
    }

    pending_ticket_ = {};
    action_pending_ = false;
    produce_decoded_ = false;
    finish_started_ = false;

    if (!result.is_ok())
    {
        return latch_failure(result);
    }

    if (!completed.is_ok())
    {
        return latch_failure(completed);
    }

    return m110::Status::success();
}

m110::Status PreparedPlayback::latch_failure(const m110::Status reason) noexcept
{
    if (first_status_.is_ok())
    {
        first_status_ = reason.is_ok() ? playback_ticket_error : reason;
    }

    stopping_ = true;

    if (configured_)
    {
        static_cast<void>(scheduler_.fail(first_status_));
    }

    if (job_ != nullptr && job_->busy() && job_->snapshot().phase != prepare::Phase::cleanup_failed)
    {
        const m110::Status cancelled = job_->cancel();

        if (cancelled.is_ok())
        {
            job_cancel_sent_ = true;
        }
    }

    return first_status_;
}

m110::Status PreparedPlayback::settle_for_stop() noexcept
{
    if (!action_pending_)
    {
        return m110::Status::success();
    }

    const m110::Status settled = scheduler_.complete(pending_ticket_, m110::Status::success(), 0U);

    if (scheduler_.snapshot().action_pending)
    {
        if (first_status_.is_ok())
        {
            first_status_ = settled.is_ok() ? playback_ticket_error : settled;
        }

        stopping_ = true;
        return first_status_;
    }

    pending_ticket_ = {};
    action_pending_ = false;
    produce_decoded_ = false;
    finish_started_ = false;
    return settled;
}

void PreparedPlayback::release_if_finished() noexcept
{
    if (job_ != nullptr && job_->busy())
    {
        return;
    }

    if (configured_)
    {
        const auto state = scheduler_.snapshot();

        if (state.action_pending)
        {
            if (!action_pending_)
            {
                const m110::Status settled = scheduler_.complete(state.pending_action, playback_ticket_error, 0U);
                first_status_ = settled.is_ok() ? playback_ticket_error : settled;
                stopping_ = true;

                if (job_ != nullptr && job_->busy())
                {
                    const m110::Status cancelled = job_->cancel();

                    if (cancelled.is_ok())
                    {
                        job_cancel_sent_ = true;
                    }
                }
            }

            return;
        }

        if (state.state != finite_playback::State::completed && state.state != finite_playback::State::failed && state.state != finite_playback::State::cancelled)
        {
            if (first_status_.is_ok())
            {
                first_status_ = playback_ticket_error;
            }

            static_cast<void>(scheduler_.fail(first_status_));
        }

        static_cast<void>(scheduler_.release());
    }

    active_ = false;
}

} // namespace waveform_generator
