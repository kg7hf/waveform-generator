#include "signal_lab/det_math.hpp"
#include "signal_lab/impairment.hpp"

#include <new>

namespace signal_lab
{
namespace
{

struct PlannedSlip
{
    std::uint64_t frame{};
    bool duplicate{};
    std::uint32_t length{};
};

class SampleSlipImpairment final : public Impairment
{
public:
    explicit SampleSlipImpairment(const SampleSlipParams& params) noexcept : params_{params} {}

    [[nodiscard]] StageType type() const noexcept override
    {
        return StageType::sample_slip;
    }

    [[nodiscard]] bool prepare(const StageContext& context, const char** error) noexcept override
    {
        scratch_ = context.scratch;
        if (scratch_ == nullptr || params_.event_count > max_events)
        {
            if (error != nullptr)
            {
                *error = "sample_slip needs scratch storage and at most 32 events";
            }
            return false;
        }
        count_ = 0U;
        max_length_ = 1U;
        for (std::uint32_t index = 0U; index < params_.event_count; ++index)
        {
            const SlipEvent& event = params_.events[index];
            if (event.length_samples == 0U || event.length_samples > max_slip_length)
            {
                if (error != nullptr)
                {
                    *error = "sample_slip length_samples must be in 1..1024 for the streaming engine";
                }
                return false;
            }
            PlannedSlip planned{det::seconds_to_frames(event.at_seconds, sample_rate_hz), event.duplicate, event.length_samples};
            // Insertion sort by frame (stable).
            std::uint32_t position = count_;
            while (position > 0U && planned_[position - 1U].frame > planned.frame)
            {
                planned_[position] = planned_[position - 1U];
                --position;
            }
            planned_[position] = planned;
            ++count_;
            if (planned.length > max_length_)
            {
                max_length_ = planned.length;
            }
        }
        if (!validate_schedule(context.total_frames, error))
        {
            return false;
        }
        cursor_ = 0U;
        delete_remaining_ = 0U;
        history_count_ = 0U;
        stats_ = StageStats{};
        stats_.events_scheduled = count_;
        return true;
    }

    [[nodiscard]] std::size_t process(float* block, std::size_t frames, std::uint64_t first_input_frame, std::size_t capacity) noexcept override
    {
        if (capacity > engine_capacity_frames)
        {
            capacity = engine_capacity_frames;
        }
        std::size_t input = 0U;
        std::size_t output = 0U;
        if (delete_remaining_ != 0U)
        {
            const std::uint64_t skip = delete_remaining_ < frames ? delete_remaining_ : frames;
            input += static_cast<std::size_t>(skip);
            delete_remaining_ -= skip;
        }
        while (cursor_ < count_ && planned_[cursor_].frame < first_input_frame + frames)
        {
            const PlannedSlip& event = planned_[cursor_];
            if (event.frame < first_input_frame + input)
            {
                stats_.events_dropped++;
                cursor_++;
                continue;
            }
            const auto before = static_cast<std::size_t>(event.frame - first_input_frame);
            output = copy_input(block, input, before, output, capacity);
            input = before;
            if (event.duplicate)
            {
                // Keep every unconsumed source sample even if a caller supplies less
                // capacity than the engine contract or skips input between calls.
                if (output + event.length + (frames - input) > capacity || output + history_count_ < event.length)
                {
                    stats_.events_dropped++;
                }
                else
                {
                    // Re-emit the last `length` delivered samples: tail of history then output so far.
                    const std::size_t from_output = output < event.length ? output : event.length;
                    const std::size_t from_history = event.length - from_output;
                    std::size_t write = output;
                    for (std::size_t index = 0U; index < from_history; ++index)
                    {
                        scratch_[write++] = history_[history_count_ - from_history + index];
                    }
                    for (std::size_t index = 0U; index < from_output; ++index)
                    {
                        scratch_[write++] = scratch_[output - from_output + index];
                    }
                    output = write;
                    stats_.events_applied++;
                }
            }
            else
            {
                const std::size_t available = frames - input;
                const std::size_t skip = event.length < available ? event.length : available;
                input += skip;
                delete_remaining_ = event.length - skip;
                stats_.events_applied++;
            }
            cursor_++;
            if (delete_remaining_ != 0U)
            {
                break;
            }
        }
        output = copy_input(block, input, frames, output, capacity);
        // Retain the last max_length_ emitted samples for future duplicates.
        const std::size_t keep = output < max_length_ ? output : max_length_;
        if (keep < max_length_ && history_count_ > 0U)
        {
            const std::size_t shift = (history_count_ + keep > max_length_) ? history_count_ + keep - max_length_ : 0U;
            for (std::size_t index = 0U; index + shift < history_count_; ++index)
            {
                history_[index] = history_[index + shift];
            }
            history_count_ -= shift;
        }
        else
        {
            history_count_ = 0U;
        }
        for (std::size_t index = 0U; index < keep; ++index)
        {
            history_[history_count_++] = scratch_[output - keep + index];
        }
        for (std::size_t index = 0U; index < output; ++index)
        {
            block[index] = scratch_[index];
        }
        stats_.component_frames += frames;
        return output;
    }

    [[nodiscard]] const StageStats& stats() const noexcept override
    {
        return stats_;
    }

private:
    bool validate_schedule(std::uint64_t total_frames, const char** error) const noexcept
    {
        // Simulate delivered history in input-frame order. Events inside a deleted
        // run are dropped by process(), so they cannot provide or consume history.
        bool applied_duplicates[max_events]{};
        std::uint64_t consumed = 0U;
        std::uint64_t history = 0U;
        for (std::uint32_t index = 0U; index < count_; ++index)
        {
            const PlannedSlip& event = planned_[index];
            if (total_frames != 0U && event.frame >= total_frames)
            {
                break;
            }
            if (event.frame < consumed)
            {
                continue;
            }
            const std::uint64_t gap = event.frame - consumed;
            history = gap >= max_slip_length - history ? max_slip_length : history + gap;
            consumed = event.frame;
            if (event.duplicate)
            {
                if (history < event.length)
                {
                    if (error != nullptr)
                    {
                        *error = "sample_slip duplicate needs length_samples previously emitted frames";
                    }
                    return false;
                }
                applied_duplicates[index] = true;
                history = event.length >= max_slip_length - history ? max_slip_length : history + event.length;
            }
            else
            {
                consumed = event.length > unbounded_frames - consumed ? unbounded_frames : consumed + event.length;
            }
        }
        // A process() call consumes up to 2048 input frames and must return all
        // output immediately. Reject excess expansion at configure time, independent
        // of the caller's block boundaries, rather than discard source samples.
        // Counting only duplicates is conservative when nearby deletions offset them.
        for (std::uint32_t first = 0U; first < count_; ++first)
        {
            if (!applied_duplicates[first])
            {
                continue;
            }
            std::size_t expansion = 0U;
            for (std::uint32_t index = first; index < count_ && planned_[index].frame - planned_[first].frame < engine_block_frames; ++index)
            {
                if (applied_duplicates[index])
                {
                    expansion += planned_[index].length;
                }
            }
            if (expansion > engine_slack_frames)
            {
                if (error != nullptr)
                {
                    *error = "sample_slip duplicates exceed 256 output frames in a 2048-input-frame window";
                }
                return false;
            }
        }
        return true;
    }

    std::size_t copy_input(const float* block, std::size_t from, std::size_t to, std::size_t output, std::size_t capacity) noexcept
    {
        for (std::size_t index = from; index < to && output < capacity; ++index)
        {
            scratch_[output++] = block[index];
        }
        return output;
    }

    SampleSlipParams params_;
    PlannedSlip planned_[max_events]{};
    std::uint32_t count_{};
    std::uint32_t cursor_{};
    std::uint64_t delete_remaining_{};
    std::uint32_t max_length_{1U};
    float history_[max_slip_length]{};
    std::size_t history_count_{};
    float* scratch_{};
    StageStats stats_{};
};

static_assert(alignof(SampleSlipImpairment) <= impairment_alignment);

} // namespace

std::size_t sample_slip_size() noexcept
{
    return align_storage(sizeof(SampleSlipImpairment));
}

Impairment* construct_sample_slip(const SampleSlipParams& params, void* storage, std::size_t storage_bytes) noexcept
{
    if (storage == nullptr || storage_bytes < sample_slip_size())
    {
        return nullptr;
    }
    return new (storage) SampleSlipImpairment(params);
}

} // namespace signal_lab
