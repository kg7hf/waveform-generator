#include "signal_lab/det_math.hpp"
#include "signal_lab/impairment.hpp"

#include <new>

namespace signal_lab
{
namespace
{

constexpr std::uint32_t max_active_fades = 8U;

class FadeImpairment final : public Impairment
{
public:
    explicit FadeImpairment(const FadeParams& params) noexcept : params_{params} {}

    [[nodiscard]] StageType type() const noexcept override
    {
        return StageType::fade;
    }

    [[nodiscard]] bool prepare(const StageContext& context, const char** error) noexcept override
    {
        if (params_.explicit_shape)
        {
            attack_ = det::seconds_to_frames(params_.attack_ms / 1e3, sample_rate_hz);
            hold_ = det::seconds_to_frames(params_.hold_ms / 1e3, sample_rate_hz);
            recovery_ = det::seconds_to_frames(params_.recovery_ms / 1e3, sample_rate_hz);
        }
        else
        {
            const std::uint64_t total = det::seconds_to_frames(params_.duration_ms / 1e3, sample_rate_hz);
            if (params_.rectangular)
            {
                attack_ = recovery_ = 0U;
                hold_ = total;
            }
            else
            {
                attack_ = recovery_ = total / 4U;
                hold_ = total - attack_ - recovery_;
            }
        }
        if (params_.rectangular)
        {
            hold_ += attack_ + recovery_;
            attack_ = recovery_ = 0U;
        }
        length_ = attack_ + hold_ + recovery_;
        if (length_ == 0U)
        {
            if (error != nullptr)
            {
                *error = "fade duration must span at least one sample";
            }
            return false;
        }
        depth_scale_ = -params_.depth_db * (det::ln10 / 20.0);
        resolve_window(params_.window, context.total_frames, window_start_, window_end_);
        total_frames_ = context.total_frames;
        rng_.reseed(context.seed, 8U, context.index);
        explicit_index_ = 0U;
        emitted_ = 0U;
        active_count_ = 0U;
        exhausted_ = false;
        position_ = static_cast<double>(window_start_);
        next_periodic_ = params_.has_first ? det::seconds_to_frames(params_.first_seconds, sample_rate_hz) : window_start_;
        stats_ = StageStats{};
        advance_pending();
        return true;
    }

    [[nodiscard]] std::size_t process(float* block, std::size_t frames, std::uint64_t first_input_frame, std::size_t) noexcept override
    {
        const std::uint64_t block_end = first_input_frame + frames;
        std::uint64_t cursor = first_input_frame;
        while (cursor < block_end)
        {
            // Retire at event boundaries, not once per input block. The bounded
            // slots represent simultaneous fades, including at a shared end/start.
            std::uint32_t kept = 0U;
            for (std::uint32_t slot = 0U; slot < active_count_; ++slot)
            {
                if (active_[slot] + length_ > cursor)
                {
                    active_[kept++] = active_[slot];
                }
            }
            active_count_ = kept;
            while (!exhausted_ && pending_ <= cursor)
            {
                if (active_count_ < max_active_fades)
                {
                    active_[active_count_++] = pending_;
                    stats_.events_applied++;
                }
                else
                {
                    stats_.events_dropped++;
                }
                advance_pending();
            }
            std::uint64_t segment_end = block_end;
            if (!exhausted_ && pending_ < segment_end)
            {
                segment_end = pending_;
            }
            for (std::uint32_t slot = 0U; slot < active_count_; ++slot)
            {
                const std::uint64_t end = active_[slot] + length_;
                if (end < segment_end)
                {
                    segment_end = end;
                }
            }
            // Keep the same chronological multiplication order for overlapping fades.
            for (std::uint32_t slot = 0U; slot < active_count_; ++slot)
            {
                const std::uint64_t start = active_[slot];
                for (std::uint64_t absolute = cursor; absolute < segment_end; ++absolute)
                {
                    const std::uint64_t u = absolute - start;
                    double depth = 1.0;
                    if (u < attack_)
                    {
                        depth = 0.5 * (1.0 - det::cos(det::pi * static_cast<double>(u) / static_cast<double>(attack_)));
                    }
                    else if (u >= attack_ + hold_)
                    {
                        const double phase = static_cast<double>(u - attack_ - hold_) / static_cast<double>(recovery_);
                        depth = 0.5 * (1.0 + det::cos(det::pi * phase));
                    }
                    const double gain = det::exp(depth_scale_ * depth);
                    float& sample = block[absolute - first_input_frame];
                    sample = static_cast<float>(static_cast<double>(sample) * gain);
                    stats_.component_frames++;
                    if (gain < minimum_gain_)
                    {
                        minimum_gain_ = gain;
                    }
                }
            }
            cursor = segment_end;
        }
        return frames;
    }

    [[nodiscard]] const StageStats& stats() const noexcept override
    {
        return stats_;
    }

private:
    void advance_pending() noexcept
    {
        if (params_.schedule == FadeSchedule::explicit_starts)
        {
            while (explicit_index_ < params_.start_count)
            {
                const std::uint64_t frame = det::seconds_to_frames(params_.starts_seconds[explicit_index_++], sample_rate_hz);
                if (total_frames_ == 0U || frame < total_frames_)
                {
                    pending_ = frame;
                    stats_.events_scheduled++;
                    return;
                }
            }
            exhausted_ = true;
            return;
        }
        if (params_.schedule == FadeSchedule::poisson)
        {
            position_ += rng_.exponential(static_cast<double>(sample_rate_hz) / params_.rate_per_sec);
            const double rounded = det::round_half_even(position_);
            if (rounded >= static_cast<double>(window_end_) || rounded < 0.0)
            {
                exhausted_ = true;
                return;
            }
            pending_ = static_cast<std::uint64_t>(rounded);
            stats_.events_scheduled++;
            return;
        }
        const std::uint64_t period = det::seconds_to_frames(params_.period_seconds, sample_rate_hz);
        if (period == 0U || next_periodic_ >= window_end_ || (params_.has_count && emitted_ >= params_.count) ||
            (total_frames_ != 0U && next_periodic_ >= total_frames_))
        {
            exhausted_ = true;
            return;
        }
        pending_ = next_periodic_;
        next_periodic_ += period;
        emitted_++;
        stats_.events_scheduled++;
    }

    FadeParams params_;
    std::uint64_t attack_{};
    std::uint64_t hold_{};
    std::uint64_t recovery_{};
    std::uint64_t length_{};
    double depth_scale_{};
    std::uint64_t window_start_{};
    std::uint64_t window_end_{};
    std::uint64_t total_frames_{};
    det::Pcg32 rng_{};
    double position_{};
    std::uint64_t next_periodic_{};
    std::uint32_t explicit_index_{};
    std::uint32_t emitted_{};
    std::uint64_t pending_{};
    bool exhausted_{};
    std::uint64_t active_[max_active_fades]{};
    std::uint32_t active_count_{};
    double minimum_gain_{1.0};
    StageStats stats_{};
};

static_assert(alignof(FadeImpairment) <= impairment_alignment);

} // namespace

std::size_t fade_size() noexcept
{
    return align_storage(sizeof(FadeImpairment));
}

Impairment* construct_fade(const FadeParams& params, void* storage, std::size_t storage_bytes) noexcept
{
    if (storage == nullptr || storage_bytes < fade_size())
    {
        return nullptr;
    }
    return new (storage) FadeImpairment(params);
}

} // namespace signal_lab
