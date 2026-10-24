// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

#include "common/finite_playback.hpp"
#include "common/pcm24_artifact.hpp"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace finite_playback = waveform_generator::finite_playback;
namespace artifact = waveform_generator::artifact;

namespace
{

inline constexpr m110::Status simulated_io_failure{.code = m110::StatusCode::io_error, .message = "simulated finite playback I/O failure"};

class MemorySink final : public artifact::Pcm24ByteSink
{
public:
    [[nodiscard]] m110::Status write_exact(const std::span<const std::uint8_t> bytes) noexcept override
    {
        if (position_ > storage_.size())
        {
            return simulated_io_failure;
        }

        if (bytes.size() > storage_.size() - position_)
        {
            storage_.resize(position_ + bytes.size());
        }

        std::copy(bytes.begin(), bytes.end(), storage_.begin() + static_cast<std::ptrdiff_t>(position_));
        position_ += bytes.size();
        return m110::Status::success();
    }

    [[nodiscard]] m110::Status seek(const std::uint64_t offset) noexcept override
    {
        if (offset > storage_.size())
        {
            return simulated_io_failure;
        }

        position_ = static_cast<std::size_t>(offset);
        return m110::Status::success();
    }

    [[nodiscard]] std::span<const std::uint8_t> bytes() const noexcept { return storage_; }

private:
    std::vector<std::uint8_t> storage_{};
    std::size_t position_{};
};

[[nodiscard]] std::array<std::uint8_t, 3U> sample_bytes(const std::uint64_t frame) noexcept
{
    const std::uint32_t sample = static_cast<std::uint32_t>((frame * 7919U + 0x54321U) & 0x00ffffffU);
    return {static_cast<std::uint8_t>(sample), static_cast<std::uint8_t>(sample >> 8U), static_cast<std::uint8_t>(sample >> 16U)};
}

[[nodiscard]] std::vector<std::uint8_t> packed_frames(const std::uint64_t first, const std::uint32_t count)
{
    std::vector<std::uint8_t> result;
    result.reserve(static_cast<std::size_t>(count) * 3U);

    for (std::uint32_t offset = 0U; offset < count; ++offset)
    {
        const auto bytes = sample_bytes(first + offset);
        result.insert(result.end(), bytes.begin(), bytes.end());
    }

    return result;
}

void complete_ok(finite_playback::Scheduler& scheduler, const finite_playback::Action action, const std::uint32_t exact_frames = 0U)
{
    REQUIRE(scheduler.complete(action, m110::Status::success(), exact_frames).is_ok());
}

void write_committed_block(finite_playback::Scheduler& scheduler, artifact::Pcm24ArtifactWriter& writer, const std::uint32_t frames)
{
    const auto produced = scheduler.begin_produce(frames);
    REQUIRE(produced.has_value());
    const auto source = packed_frames(produced.value().first_frame, produced.value().frames);
    complete_ok(scheduler, produced.value(), produced.value().frames);
    const auto write = scheduler.begin_write(frames);
    REQUIRE(write.has_value());
    REQUIRE(write.value().first_frame == produced.value().first_frame);
    REQUIRE(writer.append_frames(source).is_ok());
    complete_ok(scheduler, write.value(), write.value().frames);
    const auto barrier = scheduler.begin_visibility_barrier();
    REQUIRE(barrier.has_value());
    complete_ok(scheduler, barrier.value());
}

void read_committed_block(finite_playback::Scheduler& scheduler, const MemorySink& sink, std::vector<std::uint8_t>& queue, const std::uint32_t frames)
{
    const auto read = scheduler.begin_read(frames);
    REQUIRE(read.has_value());
    const auto begin = 44U + static_cast<std::size_t>(read.value().first_frame) * 3U;
    const auto bytes = static_cast<std::size_t>(read.value().frames) * 3U;
    const auto sink_bytes = sink.bytes();
    REQUIRE(begin + bytes <= sink_bytes.size());
    queue.insert(queue.end(), sink_bytes.begin() + static_cast<std::ptrdiff_t>(begin), sink_bytes.begin() + static_cast<std::ptrdiff_t>(begin + bytes));
    complete_ok(scheduler, read.value(), read.value().frames);
}

void consume_queued(finite_playback::Scheduler& scheduler, std::vector<std::uint8_t>& queue, std::vector<std::uint8_t>& played, const std::uint32_t frames)
{
    const auto bytes = static_cast<std::size_t>(frames) * 3U;
    REQUIRE(queue.size() >= bytes);
    REQUIRE(scheduler.consume(frames).is_ok());
    played.insert(played.end(), queue.begin(), queue.begin() + static_cast<std::ptrdiff_t>(bytes));
    queue.erase(queue.begin(), queue.begin() + static_cast<std::ptrdiff_t>(bytes));
}

struct ScheduledOutcome
{
    finite_playback::Snapshot final_state{};
    std::vector<std::uint8_t> played{};
    std::vector<finite_playback::ActionKind> actions{};
    std::uint64_t elapsed_frames{};
    std::size_t start_action_index{};
    bool started{};
    bool iteration_limit{};
};

struct VirtualTiming
{
    std::uint32_t produce_frames{};
    std::uint32_t write_frames{};
    std::uint32_t barrier_frames{};
    std::uint32_t read_frames{};
};

[[nodiscard]] std::uint32_t virtual_action_frames(const finite_playback::Action action, const VirtualTiming timing) noexcept
{
    switch (action.kind)
    {
    case finite_playback::ActionKind::produce:
        return timing.produce_frames;

    case finite_playback::ActionKind::write:
        return timing.write_frames;

    case finite_playback::ActionKind::visibility_barrier:
        return timing.barrier_frames;

    case finite_playback::ActionKind::read:
        return timing.read_frames;

    case finite_playback::ActionKind::finalize:
        return 0U;
    }

    return 0U;
}

[[nodiscard]] ScheduledOutcome run_scheduled(const finite_playback::Config config, const VirtualTiming timing, const bool inject_long_write_stall)
{
    finite_playback::Scheduler scheduler;
    REQUIRE(scheduler.configure(config).is_ok());
    MemorySink sink;
    artifact::Pcm24ArtifactWriter writer;
    REQUIRE(writer.begin(sink, {.sample_rate_hz = 48000U, .channels = 1U}).is_ok());
    std::vector<std::uint8_t> queue;
    std::vector<std::uint8_t> played;
    std::vector<std::uint8_t> staging;
    std::uint64_t staging_first_frame{};
    std::uint64_t elapsed_frames{};
    bool started{};
    bool injected_stall{};
    const auto consume_clock = [&](const std::uint32_t requested) -> bool
    {
        std::uint32_t remaining = requested;

        while (remaining > 0U)
        {
            const auto available = static_cast<std::uint32_t>(queue.size() / 3U);

            if (available == 0U)
            {
                static_cast<void>(scheduler.consume(1U));
                return false;
            }

            const auto count = std::min({remaining, config.max_block_frames, available});
            const auto status = scheduler.consume(count);

            if (!status.is_ok())
            {
                return false;
            }

            const auto bytes = static_cast<std::size_t>(count) * 3U;
            REQUIRE(queue.size() >= bytes);
            played.insert(played.end(), queue.begin(), queue.begin() + static_cast<std::ptrdiff_t>(bytes));
            queue.erase(queue.begin(), queue.begin() + static_cast<std::ptrdiff_t>(bytes));
            elapsed_frames += count;
            remaining -= count;
        }

        return true;
    };
    ScheduledOutcome outcome{};
    constexpr std::uint32_t iteration_limit = 30000U;

    for (std::uint32_t iteration = 0U; iteration < iteration_limit; ++iteration)
    {
        auto state = scheduler.snapshot();

        if (state.state == finite_playback::State::completed || state.state == finite_playback::State::failed || state.state == finite_playback::State::cancelled)
        {
            break;
        }

        const auto selected = scheduler.next_action();

        if (!selected.has_value())
        {
            REQUIRE(selected.error().code == m110::StatusCode::busy);

            if (!started)
            {
                const auto prefill = std::min<std::uint64_t>(config.queue_capacity_frames, config.total_frames);
                REQUIRE(state.committed_frames >= std::max(state.required_lead_frames, prefill));
                REQUIRE(state.read_frames - state.consumed_frames >= prefill);
                outcome.start_action_index = outcome.actions.size();
                REQUIRE(scheduler.start().is_ok());
                started = true;
                continue;
            }

            if (state.consumed_frames < state.total_frames)
            {
                REQUIRE_FALSE(queue.empty());

                if (!consume_clock(1U))
                {
                    outcome.final_state = scheduler.snapshot();
                    outcome.played = std::move(played);
                    outcome.elapsed_frames = elapsed_frames;
                    outcome.started = started;
                    return outcome;
                }

                continue;
            }

            FAIL("finite playback waited after all output was consumed");
        }

        const auto action = selected.value();
        outcome.actions.push_back(action.kind);
        auto action_cost = virtual_action_frames(action, timing);

        if (inject_long_write_stall && started && !injected_stall && action.kind == finite_playback::ActionKind::write)
        {
            action_cost += config.queue_capacity_frames + 1U;
            injected_stall = true;
        }

        if (started && action_cost > 0U && !consume_clock(action_cost))
        {
            static_cast<void>(scheduler.complete(action, m110::Status::success(),
                                                 action.kind == finite_playback::ActionKind::visibility_barrier || action.kind == finite_playback::ActionKind::finalize ? 0U : action.frames));
            break;
        }

        if (!started)
        {
            elapsed_frames += action_cost;
        }

        m110::Status operation_status = m110::Status::success();
        std::uint32_t completed_frames{};

        switch (action.kind)
        {
        case finite_playback::ActionKind::produce:
            staging = packed_frames(action.first_frame, action.frames);
            staging_first_frame = action.first_frame;
            completed_frames = action.frames;
            break;

        case finite_playback::ActionKind::write:
            REQUIRE(staging_first_frame == action.first_frame);
            REQUIRE(staging.size() >= static_cast<std::size_t>(action.frames) * 3U);
            operation_status = writer.append_frames(std::span<const std::uint8_t>{staging}.first(static_cast<std::size_t>(action.frames) * 3U));

            if (operation_status.is_ok())
            {
                staging.erase(staging.begin(), staging.begin() + static_cast<std::ptrdiff_t>(static_cast<std::size_t>(action.frames) * 3U));
                staging_first_frame += action.frames;
                completed_frames = action.frames;
            }

            break;

        case finite_playback::ActionKind::visibility_barrier:
            break;

        case finite_playback::ActionKind::read:
        {
            const auto sink_data = sink.bytes();
            const auto offset = 44U + static_cast<std::size_t>(action.first_frame) * 3U;
            const auto length = static_cast<std::size_t>(action.frames) * 3U;
            REQUIRE(offset + length <= sink_data.size());
            queue.insert(queue.end(), sink_data.begin() + static_cast<std::ptrdiff_t>(offset), sink_data.begin() + static_cast<std::ptrdiff_t>(offset + length));
            completed_frames = action.frames;
            break;
        }

        case finite_playback::ActionKind::finalize:
        {
            artifact::Pcm24ArtifactSummary summary{};
            operation_status = writer.finish(summary);
            break;
        }
        }

        const auto completion = scheduler.complete(action, operation_status, completed_frames);

        if (!completion.is_ok() && scheduler.snapshot().state != finite_playback::State::failed)
        {
            FAIL("finite playback action completion failed unexpectedly");
        }
    }

    outcome.final_state = scheduler.snapshot();
    outcome.played = std::move(played);
    outcome.elapsed_frames = elapsed_frames;
    outcome.started = started;
    outcome.iteration_limit =
        outcome.final_state.state != finite_playback::State::completed && outcome.final_state.state != finite_playback::State::failed && outcome.final_state.state != finite_playback::State::cancelled;
    return outcome;
}

} // namespace

TEST_CASE("finite playback keeps a slower finite producer audible and byte exact", "[finite-playback]")
{
    constexpr std::uint64_t total_frames = 9600U;
    constexpr std::uint32_t queue_capacity = 512U;
    constexpr std::uint32_t max_block = 64U;
    finite_playback::Scheduler scheduler;
    const finite_playback::Config config{
        .total_frames = total_frames, .measured_rate_frames_per_second = 24000U, .margin_frames = 256U, .queue_capacity_frames = queue_capacity, .max_block_frames = max_block};
    REQUIRE(scheduler.configure(config).is_ok());
    MemorySink sink;
    artifact::Pcm24ArtifactWriter writer;
    REQUIRE(writer.begin(sink, {.sample_rate_hz = finite_playback::playback_sample_rate_hz, .channels = 1U}).is_ok());
    const auto lead = scheduler.snapshot().required_lead_frames;
    REQUIRE(lead > queue_capacity);
    REQUIRE(lead < total_frames);

    while (scheduler.snapshot().produced_frames < lead)
    {
        const auto remaining = lead - scheduler.snapshot().produced_frames;
        write_committed_block(scheduler, writer, static_cast<std::uint32_t>(std::min<std::uint64_t>(max_block, remaining)));
    }

    std::vector<std::uint8_t> queue;
    std::vector<std::uint8_t> played;

    while (queue.size() / 3U < queue_capacity)
    {
        const auto state = scheduler.snapshot();
        const auto queued_frames = static_cast<std::uint32_t>(queue.size() / 3U);
        const auto room = queue_capacity - queued_frames;
        const auto available = state.committed_frames - state.read_frames;
        REQUIRE(available > 0U);
        const auto count = static_cast<std::uint32_t>(std::min<std::uint64_t>({max_block, room, available}));
        read_committed_block(scheduler, sink, queue, count);
    }

    REQUIRE(scheduler.snapshot().read_frames == queue_capacity);
    REQUIRE(scheduler.start().is_ok());

    while (scheduler.snapshot().produced_frames < total_frames)
    {
        const auto remaining = total_frames - scheduler.snapshot().produced_frames;
        const auto frames = static_cast<std::uint32_t>(std::min<std::uint64_t>(max_block, remaining));
        // The measured end-to-end rate is half the playback rate. Each bounded
        // producer/write action occupies half of this block's virtual time;
        // playback continues while those operations are pending.
        const auto produce = scheduler.begin_produce(frames);
        REQUIRE(produce.has_value());
        const auto source = packed_frames(produce.value().first_frame, produce.value().frames);
        consume_queued(scheduler, queue, played, frames);
        complete_ok(scheduler, produce.value(), produce.value().frames);
        const auto write = scheduler.begin_write(frames);
        REQUIRE(write.has_value());
        REQUIRE(write.value().first_frame == produce.value().first_frame);
        REQUIRE(writer.append_frames(source).is_ok());
        consume_queued(scheduler, queue, played, frames);
        complete_ok(scheduler, write.value(), write.value().frames);
        const auto barrier = scheduler.begin_visibility_barrier();
        REQUIRE(barrier.has_value());
        complete_ok(scheduler, barrier.value());

        while (queue.size() / 3U < queue_capacity)
        {
            const auto state = scheduler.snapshot();
            const auto room = queue_capacity - static_cast<std::uint32_t>(queue.size() / 3U);
            const auto available = state.committed_frames - state.read_frames;

            if (available == 0U)
            {
                break;
            }

            const auto count = static_cast<std::uint32_t>(std::min<std::uint64_t>({max_block, room, available}));
            read_committed_block(scheduler, sink, queue, count);
        }

        REQUIRE(queue.size() / 3U <= queue_capacity);
    }

    while (scheduler.snapshot().consumed_frames < total_frames)
    {
        const auto state = scheduler.snapshot();
        const auto remaining = total_frames - state.consumed_frames;

        if (queue.empty())
        {
            const auto available = state.committed_frames - state.read_frames;
            REQUIRE(available > 0U);
            const auto count = static_cast<std::uint32_t>(std::min<std::uint64_t>(max_block, available));
            read_committed_block(scheduler, sink, queue, count);
        }

        const auto queued = static_cast<std::uint32_t>(queue.size() / 3U);
        consume_queued(scheduler, queue, played, static_cast<std::uint32_t>(std::min<std::uint64_t>({max_block, remaining, queued})));
    }

    const auto finalize = scheduler.begin_finalize();
    REQUIRE(finalize.has_value());
    artifact::Pcm24ArtifactSummary summary{};
    REQUIRE(writer.finish(summary).is_ok());
    complete_ok(scheduler, finalize.value());
    std::vector<std::uint8_t> expected = packed_frames(0U, static_cast<std::uint32_t>(total_frames));
    const auto state = scheduler.snapshot();
    CHECK(state.state == finite_playback::State::completed);
    CHECK(state.artifact_finalized);
    CHECK(state.produced_frames == total_frames);
    CHECK(state.written_frames == total_frames);
    CHECK(state.committed_frames == total_frames);
    CHECK(state.read_frames == total_frames);
    CHECK(state.consumed_frames == total_frames);
    CHECK(played == expected);
    const auto sink_bytes = sink.bytes();
    REQUIRE(sink_bytes.size() >= 44U + expected.size());
    CHECK(std::equal(expected.begin(), expected.end(), sink_bytes.begin() + 44));
    CHECK(summary.frames == total_frames);
}

TEST_CASE("finite playback enforces commit watermarks, action bounds and queue backpressure", "[finite-playback]")
{
    finite_playback::Scheduler scheduler;
    REQUIRE(scheduler.configure({.total_frames = 8U, .measured_rate_frames_per_second = 48000U, .margin_frames = 0U, .queue_capacity_frames = 4U, .max_block_frames = 4U}).is_ok());
    MemorySink sink;
    artifact::Pcm24ArtifactWriter writer;
    REQUIRE(writer.begin(sink, {.sample_rate_hz = 48000U, .channels = 1U}).is_ok());
    std::vector<std::uint8_t> queue;
    std::vector<std::uint8_t> played;
    write_committed_block(scheduler, writer, 4U);
    CHECK(scheduler.snapshot().committed_frames == 4U);
    const auto over_read = scheduler.begin_read(5U);
    CHECK_FALSE(over_read.has_value());
    const auto read = scheduler.begin_read(4U);
    REQUIRE(read.has_value());
    const auto sink_bytes = sink.bytes();
    const auto read_bytes = sink_bytes.size() - 44U;
    queue.insert(queue.end(), sink_bytes.begin() + 44, sink_bytes.begin() + static_cast<std::ptrdiff_t>(44U + read_bytes));
    complete_ok(scheduler, read.value(), 4U);
    CHECK(scheduler.start().is_ok());
    write_committed_block(scheduler, writer, 4U);
    const auto full = scheduler.begin_read(1U);
    CHECK_FALSE(full.has_value());
    CHECK(full.error().code == m110::StatusCode::busy);
    CHECK(scheduler.snapshot().read_frames == 4U);
    consume_queued(scheduler, queue, played, 2U);
    const auto pending_read = scheduler.begin_read(2U);
    REQUIRE(pending_read.has_value());
    consume_queued(scheduler, queue, played, 2U);
    const auto next_bytes = packed_frames(pending_read.value().first_frame, pending_read.value().frames);
    const auto sink_bytes_after_read = sink.bytes();
    REQUIRE(std::equal(next_bytes.begin(), next_bytes.end(), sink_bytes_after_read.begin() + static_cast<std::ptrdiff_t>(44U + pending_read.value().first_frame * 3U)));
    queue.insert(queue.end(), next_bytes.begin(), next_bytes.end());
    complete_ok(scheduler, pending_read.value(), 2U);
    CHECK(scheduler.snapshot().read_frames == 6U);
    CHECK(scheduler.snapshot().consumed_frames == 4U);
    const auto remaining_read = scheduler.begin_read(2U);
    REQUIRE(remaining_read.has_value());
    const auto final_bytes = packed_frames(remaining_read.value().first_frame, remaining_read.value().frames);
    queue.insert(queue.end(), final_bytes.begin(), final_bytes.end());
    complete_ok(scheduler, remaining_read.value(), 2U);
    consume_queued(scheduler, queue, played, 4U);
    const auto finalize = scheduler.begin_finalize();
    REQUIRE(finalize.has_value());
    artifact::Pcm24ArtifactSummary summary{};
    REQUIRE(writer.finish(summary).is_ok());
    complete_ok(scheduler, finalize.value());
    CHECK(played == packed_frames(0U, 8U));
}

TEST_CASE("finite playback rejects underruns and latches I/O failures without advancing watermarks", "[finite-playback]")
{
    finite_playback::Scheduler underrun;
    REQUIRE(underrun.configure({.total_frames = 4U, .measured_rate_frames_per_second = 48000U, .margin_frames = 0U, .queue_capacity_frames = 4U, .max_block_frames = 4U}).is_ok());
    const auto produced = underrun.begin_produce(4U);
    REQUIRE(produced.has_value());
    complete_ok(underrun, produced.value(), 4U);
    const auto write = underrun.begin_write(4U);
    REQUIRE(write.has_value());
    complete_ok(underrun, write.value(), 4U);
    const auto barrier = underrun.begin_visibility_barrier();
    REQUIRE(barrier.has_value());
    complete_ok(underrun, barrier.value());
    const auto read = underrun.begin_read(4U);
    REQUIRE(read.has_value());
    complete_ok(underrun, read.value(), 4U);
    REQUIRE(underrun.start().is_ok());
    REQUIRE(underrun.consume(4U).is_ok());
    const auto short_queue = underrun.consume(1U);
    CHECK(short_queue.code == m110::StatusCode::timeout);
    CHECK(underrun.snapshot().state == finite_playback::State::failed);
    CHECK(underrun.snapshot().consumed_frames == 4U);
    CHECK(underrun.snapshot().first_status.message == short_queue.message);
    finite_playback::Scheduler failed_write;
    REQUIRE(failed_write.configure({.total_frames = 8U, .measured_rate_frames_per_second = 48000U, .margin_frames = 0U, .queue_capacity_frames = 4U, .max_block_frames = 4U}).is_ok());
    const auto block = failed_write.begin_produce(4U);
    REQUIRE(block.has_value());
    complete_ok(failed_write, block.value(), 4U);
    const auto failed_action = failed_write.begin_write(4U);
    REQUIRE(failed_action.has_value());
    const auto first = failed_write.complete(failed_action.value(), simulated_io_failure);
    CHECK(first.message == simulated_io_failure.message);
    CHECK(failed_write.snapshot().state == finite_playback::State::failed);
    CHECK(failed_write.snapshot().written_frames == 0U);
    CHECK(failed_write.snapshot().committed_frames == 0U);
    CHECK(failed_write.fail({.code = m110::StatusCode::io_error, .message = "later failure"}).message == first.message);
    CHECK_FALSE(failed_write.configure({.total_frames = 8U, .measured_rate_frames_per_second = 48000U, .margin_frames = 0U, .queue_capacity_frames = 4U, .max_block_frames = 4U}).is_ok());
    REQUIRE(failed_write.release().is_ok());
    REQUIRE(failed_write.configure({.total_frames = 8U, .measured_rate_frames_per_second = 48000U, .margin_frames = 0U, .queue_capacity_frames = 4U, .max_block_frames = 4U}).is_ok());
    finite_playback::Scheduler failed_visibility;
    REQUIRE(failed_visibility.configure({.total_frames = 4U, .measured_rate_frames_per_second = 48000U, .margin_frames = 0U, .queue_capacity_frames = 4U, .max_block_frames = 4U}).is_ok());
    const auto visibility_block = failed_visibility.begin_produce(4U);
    REQUIRE(visibility_block.has_value());
    complete_ok(failed_visibility, visibility_block.value(), 4U);
    const auto visibility_write = failed_visibility.begin_write(4U);
    REQUIRE(visibility_write.has_value());
    complete_ok(failed_visibility, visibility_write.value(), 4U);
    const auto failed_barrier = failed_visibility.begin_visibility_barrier();
    REQUIRE(failed_barrier.has_value());
    REQUIRE_FALSE(failed_visibility.complete(failed_barrier.value(), simulated_io_failure).is_ok());
    CHECK(failed_visibility.snapshot().committed_frames == 0U);
    CHECK(failed_visibility.snapshot().state == finite_playback::State::failed);
}

TEST_CASE("finite playback cancellation waits for action acknowledgement and rejects stale tickets", "[finite-playback]")
{
    finite_playback::Scheduler scheduler;
    CHECK_FALSE(scheduler.configure({.total_frames = 0U, .measured_rate_frames_per_second = 48000U, .margin_frames = 0U, .queue_capacity_frames = 4U, .max_block_frames = 4U}).is_ok());
    REQUIRE(scheduler.configure({.total_frames = 4U, .measured_rate_frames_per_second = 48000U, .margin_frames = 0U, .queue_capacity_frames = 4U, .max_block_frames = 4U}).is_ok());
    const auto pending = scheduler.begin_produce(4U);
    REQUIRE(pending.has_value());
    CHECK(scheduler.release().code == m110::StatusCode::busy);
    const auto cancel_status = scheduler.cancel();
    CHECK(cancel_status.code == m110::StatusCode::unavailable);
    CHECK(scheduler.snapshot().state == finite_playback::State::cancelling);
    CHECK(scheduler.snapshot().action_pending);
    CHECK_FALSE(scheduler.configure({.total_frames = 4U, .measured_rate_frames_per_second = 48000U, .margin_frames = 0U, .queue_capacity_frames = 4U, .max_block_frames = 4U}).is_ok());
    const auto acknowledgement = scheduler.complete(pending.value(), m110::Status::success(), pending.value().frames);
    CHECK(acknowledgement.message == cancel_status.message);
    CHECK(scheduler.snapshot().state == finite_playback::State::cancelled);
    CHECK_FALSE(scheduler.snapshot().action_pending);
    CHECK(scheduler.snapshot().produced_frames == 0U);
    CHECK_FALSE(scheduler.configure({.total_frames = 4U, .measured_rate_frames_per_second = 48000U, .margin_frames = 0U, .queue_capacity_frames = 4U, .max_block_frames = 4U}).is_ok());
    const auto duplicate = scheduler.complete(pending.value(), m110::Status::success(), pending.value().frames);
    CHECK(duplicate.code == m110::StatusCode::invalid_argument);
    CHECK(scheduler.snapshot().produced_frames == 0U);
}

TEST_CASE("finite playback validates ranges and finalization failure remains incomplete", "[finite-playback]")
{
    finite_playback::Scheduler scheduler;
    REQUIRE(scheduler.configure({.total_frames = 4U, .measured_rate_frames_per_second = 48000U, .margin_frames = 0U, .queue_capacity_frames = 4U, .max_block_frames = 4U}).is_ok());
    CHECK_FALSE(scheduler.begin_produce(0U).has_value());
    CHECK_FALSE(scheduler.begin_produce(5U).has_value());
    CHECK_FALSE(scheduler.begin_write(1U).has_value());
    CHECK_FALSE(scheduler.begin_read(1U).has_value());
    CHECK(scheduler.snapshot().produced_frames == 0U);
    const auto produced = scheduler.begin_produce(4U);
    REQUIRE(produced.has_value());
    complete_ok(scheduler, produced.value(), 4U);
    const auto write = scheduler.begin_write(4U);
    REQUIRE(write.has_value());
    complete_ok(scheduler, write.value(), 4U);
    const auto barrier = scheduler.begin_visibility_barrier();
    REQUIRE(barrier.has_value());
    complete_ok(scheduler, barrier.value());
    const auto read = scheduler.begin_read(4U);
    REQUIRE(read.has_value());
    complete_ok(scheduler, read.value(), 4U);
    REQUIRE(scheduler.start().is_ok());
    REQUIRE(scheduler.consume(4U).is_ok());
    const auto finalize = scheduler.begin_finalize();
    REQUIRE(finalize.has_value());
    REQUIRE_FALSE(scheduler.complete(finalize.value(), simulated_io_failure).is_ok());
    CHECK(scheduler.snapshot().state == finite_playback::State::failed);
    CHECK_FALSE(scheduler.snapshot().artifact_finalized);
    CHECK(scheduler.snapshot().first_status.message == simulated_io_failure.message);
}

TEST_CASE("finite playback failure and release preserve terminal status and ticket ownership", "[finite-playback]")
{
    finite_playback::Scheduler scheduler;
    const auto before_configure = scheduler.snapshot();
    const auto premature_failure = scheduler.fail(m110::Status::success());
    CHECK(premature_failure.code == m110::StatusCode::invalid_configuration);
    CHECK(scheduler.snapshot().state == before_configure.state);
    CHECK(scheduler.snapshot().first_status.is_ok());
    REQUIRE(scheduler.configure({.total_frames = 1U, .measured_rate_frames_per_second = 48000U, .margin_frames = 0U, .queue_capacity_frames = 1U, .max_block_frames = 1U}).is_ok());
    const auto produce = scheduler.begin_produce(1U);
    REQUIRE(produce.has_value());
    complete_ok(scheduler, produce.value(), 1U);
    const auto write = scheduler.begin_write(1U);
    REQUIRE(write.has_value());
    complete_ok(scheduler, write.value(), 1U);
    const auto barrier = scheduler.begin_visibility_barrier();
    REQUIRE(barrier.has_value());
    complete_ok(scheduler, barrier.value());
    const auto read = scheduler.begin_read(1U);
    REQUIRE(read.has_value());
    complete_ok(scheduler, read.value(), 1U);
    REQUIRE(scheduler.start().is_ok());
    REQUIRE(scheduler.consume(1U).is_ok());
    const auto finalize = scheduler.begin_finalize();
    REQUIRE(finalize.has_value());
    complete_ok(scheduler, finalize.value());
    const auto completed = scheduler.snapshot();
    CHECK(completed.state == finite_playback::State::completed);
    CHECK(completed.first_status.is_ok());
    CHECK_FALSE(completed.owner_released);
    const auto fail_after_completion = scheduler.fail(m110::Status::success());
    CHECK(fail_after_completion.is_ok());
    CHECK(scheduler.snapshot().state == finite_playback::State::completed);
    CHECK(scheduler.snapshot().artifact_finalized);
    CHECK(scheduler.snapshot().first_status.is_ok());
    CHECK_FALSE(scheduler.configure({.total_frames = 1U, .measured_rate_frames_per_second = 48000U, .margin_frames = 0U, .queue_capacity_frames = 1U, .max_block_frames = 1U}).is_ok());
    REQUIRE(scheduler.release().is_ok());
    CHECK(scheduler.snapshot().owner_released);
    REQUIRE(scheduler.configure({.total_frames = 1U, .measured_rate_frames_per_second = 48000U, .margin_frames = 0U, .queue_capacity_frames = 1U, .max_block_frames = 1U}).is_ok());
    const auto stale_ticket = scheduler.complete(finalize.value(), m110::Status::success());
    CHECK(stale_ticket.code == m110::StatusCode::invalid_argument);
    CHECK(scheduler.snapshot().state == finite_playback::State::ready);
    CHECK(scheduler.snapshot().produced_frames == 0U);
    finite_playback::Scheduler cancelled;
    REQUIRE(cancelled.configure({.total_frames = 2U, .measured_rate_frames_per_second = 48000U, .margin_frames = 0U, .queue_capacity_frames = 2U, .max_block_frames = 2U}).is_ok());
    const auto cancellation = cancelled.cancel();
    CHECK(cancellation.code == m110::StatusCode::unavailable);
    const auto cancelled_state = cancelled.snapshot();
    const auto fail_after_cancel = cancelled.fail(m110::Status::success());
    CHECK(fail_after_cancel.code == cancelled_state.first_status.code);
    CHECK(fail_after_cancel.message == cancelled_state.first_status.message);
    CHECK(cancelled.snapshot().state == finite_playback::State::cancelled);
    CHECK(cancelled.snapshot().first_status.code == cancelled_state.first_status.code);
    CHECK(cancelled.snapshot().first_status.message == cancelled_state.first_status.message);
    CHECK_FALSE(cancelled.snapshot().owner_released);
    REQUIRE(cancelled.release().is_ok());
    CHECK(cancelled.snapshot().owner_released);
}

TEST_CASE("finite playback lead arithmetic saturates safely at uint64 boundaries", "[finite-playback]")
{
    constexpr std::uint64_t maximum = std::numeric_limits<std::uint64_t>::max();
    finite_playback::Scheduler slow_rate;
    REQUIRE(slow_rate.configure({.total_frames = maximum, .measured_rate_frames_per_second = 1U, .margin_frames = 0U, .queue_capacity_frames = 1U, .max_block_frames = 1U}).is_ok());
    // For a one-frame/second measured rate, the missing fraction is 1/48000
    // of the total. This form independently computes ceil(N*47999/48000)
    // without multiplying two uint64 values.
    CHECK(slow_rate.snapshot().required_lead_frames == maximum - maximum / 48000U);
    finite_playback::Scheduler full_rate_with_margin;
    REQUIRE(
        full_rate_with_margin.configure({.total_frames = maximum, .measured_rate_frames_per_second = 48000U, .margin_frames = maximum, .queue_capacity_frames = 1U, .max_block_frames = 1U}).is_ok());
    CHECK(full_rate_with_margin.snapshot().required_lead_frames == maximum);
    finite_playback::Scheduler zero_margin_at_full_rate;
    REQUIRE(zero_margin_at_full_rate.configure({.total_frames = maximum, .measured_rate_frames_per_second = 48000U, .margin_frames = 0U, .queue_capacity_frames = 1U, .max_block_frames = 1U}).is_ok());
    CHECK(zero_margin_at_full_rate.snapshot().required_lead_frames == 0U);
    finite_playback::Scheduler invalid;
    CHECK_FALSE(invalid.configure({.total_frames = 1U, .measured_rate_frames_per_second = 0U, .margin_frames = 0U, .queue_capacity_frames = 1U, .max_block_frames = 1U}).is_ok());
    CHECK_FALSE(invalid.configure({.total_frames = 1U, .measured_rate_frames_per_second = 48000U, .margin_frames = 0U, .queue_capacity_frames = 0U, .max_block_frames = 0U}).is_ok());
    CHECK_FALSE(invalid.configure({.total_frames = 1U, .measured_rate_frames_per_second = 48000U, .margin_frames = 0U, .queue_capacity_frames = 1U, .max_block_frames = 2U}).is_ok());
    CHECK(invalid.snapshot().state == finite_playback::State::unconfigured);
}

TEST_CASE("finite playback preserves cursors when an in-flight visibility or read action loses its queue", "[finite-playback]")
{
    const auto config = finite_playback::Config{.total_frames = 4U, .measured_rate_frames_per_second = 48000U, .margin_frames = 0U, .queue_capacity_frames = 2U, .max_block_frames = 2U};
    finite_playback::Scheduler visibility_pending;
    REQUIRE(visibility_pending.configure(config).is_ok());
    MemorySink visibility_sink;
    artifact::Pcm24ArtifactWriter visibility_writer;
    REQUIRE(visibility_writer.begin(visibility_sink, {.sample_rate_hz = 48000U, .channels = 1U}).is_ok());
    write_committed_block(visibility_pending, visibility_writer, 2U);
    std::vector<std::uint8_t> visibility_queue;
    read_committed_block(visibility_pending, visibility_sink, visibility_queue, 2U);
    REQUIRE(visibility_pending.start().is_ok());
    REQUIRE(visibility_pending.consume(2U).is_ok());
    const auto visibility_produce = visibility_pending.begin_produce(2U);
    REQUIRE(visibility_produce.has_value());
    const auto visibility_source = packed_frames(visibility_produce.value().first_frame, 2U);
    complete_ok(visibility_pending, visibility_produce.value(), 2U);
    const auto visibility_write = visibility_pending.begin_write(2U);
    REQUIRE(visibility_write.has_value());
    REQUIRE(visibility_writer.append_frames(visibility_source).is_ok());
    complete_ok(visibility_pending, visibility_write.value(), 2U);
    const auto barrier = visibility_pending.begin_visibility_barrier();
    REQUIRE(barrier.has_value());
    const auto visibility_underrun = visibility_pending.consume(1U);
    CHECK(visibility_underrun.code == m110::StatusCode::timeout);
    const auto barrier_ack = visibility_pending.complete(barrier.value(), m110::Status::success());
    CHECK(barrier_ack.message == visibility_underrun.message);
    CHECK(visibility_pending.snapshot().committed_frames == 2U);
    CHECK(visibility_pending.snapshot().consumed_frames == 2U);
    CHECK_FALSE(visibility_pending.snapshot().action_pending);
    finite_playback::Scheduler read_pending;
    REQUIRE(read_pending.configure(config).is_ok());
    MemorySink read_sink;
    artifact::Pcm24ArtifactWriter read_writer;
    REQUIRE(read_writer.begin(read_sink, {.sample_rate_hz = 48000U, .channels = 1U}).is_ok());
    write_committed_block(read_pending, read_writer, 2U);
    std::vector<std::uint8_t> read_queue;
    std::vector<std::uint8_t> played;
    read_committed_block(read_pending, read_sink, read_queue, 2U);
    REQUIRE(read_pending.start().is_ok());
    consume_queued(read_pending, read_queue, played, 2U);
    write_committed_block(read_pending, read_writer, 2U);
    const auto pending_read = read_pending.begin_read(2U);
    REQUIRE(pending_read.has_value());
    const auto read_underrun = read_pending.consume(1U);
    CHECK(read_underrun.code == m110::StatusCode::timeout);
    const auto read_ack = read_pending.complete(pending_read.value(), m110::Status::success(), 2U);
    CHECK(read_ack.message == read_underrun.message);
    CHECK(read_pending.snapshot().read_frames == 2U);
    CHECK(read_pending.snapshot().consumed_frames == 2U);
    CHECK_FALSE(read_pending.snapshot().action_pending);
}

TEST_CASE("finite playback next-action scheduling overlaps bounded I/O with the frame clock", "[finite-playback]")
{
    constexpr std::uint64_t slow_total = 4096U;
    constexpr std::uint32_t slow_queue = 64U;
    constexpr std::uint32_t slow_block = 16U;
    // One 16-frame producer block costs 16 clock frames; its write, visibility
    // barrier, and refill cost 4, 4, and 1. Reads consume 1/16 of the
    // single-owner clock, leaving 15/16 for production/write/barrier:
    // 48000 * 15/24 = 30000 frames/s, above the conservative declared 24000.
    // These synthetic durations are not RT1170 performance measurements.
    const auto slow = run_scheduled({.total_frames = slow_total, .measured_rate_frames_per_second = 24000U, .margin_frames = 256U, .queue_capacity_frames = slow_queue, .max_block_frames = slow_block},
                                    {.produce_frames = 16U, .write_frames = 4U, .barrier_frames = 4U, .read_frames = 1U}, false);
    CHECK(slow.started);
    CHECK_FALSE(slow.iteration_limit);
    CHECK(slow.final_state.state == finite_playback::State::completed);
    CHECK(slow.final_state.required_lead_frames > slow_queue);
    CHECK(slow.final_state.produced_frames > slow.final_state.required_lead_frames);
    CHECK(slow.elapsed_frames > 0U);
    REQUIRE(slow.start_action_index < slow.actions.size());
    CHECK(slow.actions[0U] == finite_playback::ActionKind::produce);
    CHECK(slow.actions[1U] == finite_playback::ActionKind::write);
    CHECK(slow.actions[2U] == finite_playback::ActionKind::visibility_barrier);
    CHECK(slow.actions[slow.start_action_index] == finite_playback::ActionKind::produce);
    REQUIRE(slow.start_action_index + 3U < slow.actions.size());
    // Rendering consumed one refill batch, which takes priority over writing.
    CHECK(slow.actions[slow.start_action_index + 1U] == finite_playback::ActionKind::read);
    CHECK(slow.actions[slow.start_action_index + 2U] == finite_playback::ActionKind::write);
    CHECK(slow.actions[slow.start_action_index + 3U] == finite_playback::ActionKind::visibility_barrier);
    CHECK(slow.actions.back() == finite_playback::ActionKind::finalize);
    CHECK(slow.played == packed_frames(0U, static_cast<std::uint32_t>(slow_total)));
    constexpr std::uint64_t fast_total = 1024U;
    // With production/write/barrier costing 2+2+2 and reads 1/16 of the
    // clock, capacity is 48000 * 15/6 = 120000 frames/s, above declared 96000.
    const auto fast = run_scheduled({.total_frames = fast_total, .measured_rate_frames_per_second = 96000U, .margin_frames = 0U, .queue_capacity_frames = 64U, .max_block_frames = 16U},
                                    {.produce_frames = 2U, .write_frames = 2U, .barrier_frames = 2U, .read_frames = 1U}, false);
    CHECK(fast.started);
    CHECK_FALSE(fast.iteration_limit);
    CHECK(fast.final_state.state == finite_playback::State::completed);
    CHECK(fast.final_state.required_lead_frames == 0U);
    REQUIRE(fast.start_action_index < fast.actions.size());
    CHECK(fast.actions[fast.start_action_index] == finite_playback::ActionKind::produce);
    CHECK(fast.played == packed_frames(0U, static_cast<std::uint32_t>(fast_total)));
    const auto insufficient_margin = run_scheduled({.total_frames = 128U, .measured_rate_frames_per_second = 48000U, .margin_frames = 0U, .queue_capacity_frames = 8U, .max_block_frames = 8U},
                                                   {.produce_frames = 1U, .write_frames = 1U, .barrier_frames = 1U, .read_frames = 1U}, true);
    CHECK(insufficient_margin.started);
    CHECK_FALSE(insufficient_margin.iteration_limit);
    CHECK(insufficient_margin.final_state.state == finite_playback::State::failed);
    CHECK(insufficient_margin.final_state.first_status.code == m110::StatusCode::timeout);
    CHECK(insufficient_margin.final_state.consumed_frames < insufficient_margin.final_state.total_frames);
    CHECK(std::find(insufficient_margin.actions.begin(), insufficient_margin.actions.end(), finite_playback::ActionKind::write) != insufficient_margin.actions.end());
}
