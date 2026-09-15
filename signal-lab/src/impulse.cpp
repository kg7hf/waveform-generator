// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Paul R. Decker

#include "band_fir.hpp"
#include "signal_lab/det_math.hpp"
#include "signal_lab/impairment.hpp"

#include <new>

namespace signal_lab
{
namespace
{

constexpr std::uint32_t max_active_events = 12U;
constexpr std::size_t chunk_frames = 256U;

struct ActiveEvent
{
    std::uint64_t start{};
    std::uint32_t ordinal{};
    double envelope{1.0}; // e^(-t/tau) carried across blocks
    double phase{};
    det::Pcg32 rng{};
};

class ImpulseImpairment final : public Impairment
{
public:
    explicit ImpulseImpairment(const ImpulseParams& params) noexcept : params_
    {
        params
    } {}

    [[nodiscard]] StageType type() const noexcept override
    {
        return StageType::impulse;
    }

    [[nodiscard]] bool prepare(const StageContext& context, const char** error) noexcept override
    {
        if (params_.ring_hz < 0.0 || params_.ring_hz >= static_cast<double>(sample_rate_hz) / 2.0)
        {
            if (error != nullptr)
            {
                *error = "impulse.ring_hz out of range";
            }

            return false;
        }

        tau_ = params_.decay_ms * 1e-3 * static_cast<double>(sample_rate_hz);
        const double length = det::round_half_even(params_.truncate_tau * tau_);
        length_ = length < 2.0 ? 2U : static_cast<std::uint64_t>(length);
        decay_ = det::exp(-1.0 / tau_);
        amplitude_ = context.reference_rms * det::db_to_amplitude(params_.peak_db);
        radians_per_frame_ = det::two_pi * params_.ring_hz / static_cast<double>(sample_rate_hz);
        resolve_window(params_.window, context.total_frames, start_, end_);
        seed_ = context.seed;
        index_ = context.index;
        schedule_rng_.reseed(context.seed, 4U, context.index);
        phase_rng_.reseed(context.seed, 5U, context.index);
        fir_.reset(nullptr, false);
        position_ = static_cast<double>(start_);
        const std::uint64_t first = params_.has_first ? det::seconds_to_frames(params_.first_seconds, sample_rate_hz) : start_;
        next_periodic_ = first > start_ ? first : start_;
        period_ = det::seconds_to_frames(params_.period_seconds, sample_rate_hz);
        scheduled_ = 0U;
        active_count_ = 0U;
        exhausted_ = false;
        stats_ = StageStats{};
        advance_pending();
        return true;
    }

    [[nodiscard]] std::size_t process(float* block, std::size_t frames, std::uint64_t first_input_frame, std::size_t) noexcept override
    {
        std::size_t done = 0U;

        while (done < frames)
        {
            const std::size_t count = (frames - done) < chunk_frames ? (frames - done) : chunk_frames;
            const std::uint64_t chunk_start = first_input_frame + done;
            const std::uint64_t chunk_end = chunk_start + count;
            admit_events(chunk_end);

            for (std::size_t index = 0U; index < count; ++index)
            {
                excitation_[index] = 0.0f;
            }

            bool any = false;
            std::uint32_t kept = 0U;

            for (std::uint32_t slot = 0U; slot < active_count_; ++slot)
            {
                ActiveEvent& event = active_[slot];
                const std::uint64_t event_end = event.start + length_;

                if (event_end <= chunk_start)
                {
                    continue;
                }

                const std::uint64_t from = event.start > chunk_start ? event.start : chunk_start;
                const std::uint64_t to = event_end < chunk_end ? event_end : chunk_end;

                for (std::uint64_t absolute = from; absolute < to; ++absolute)
                {
                    const std::uint64_t t = absolute - event.start;
                    double ring = 1.0;

                    if (params_.ring_noise)
                    {
                        ring = event.rng.gaussian();
                    }
                    else if (params_.ring_hz > 0.0)
                    {
                        ring = det::cos(radians_per_frame_ * static_cast<double>(t) + event.phase);
                    }

                    excitation_[absolute - chunk_start] += static_cast<float>(amplitude_ * event.envelope * ring);
                    event.envelope *= decay_;
                }

                any = true;
                active_[kept++] = event;
            }

            active_count_ = kept;

            if (any || params_.ring_noise)
            {
                const float* added = excitation_;

                if (params_.ring_noise)
                {
                    fir_.filter(excitation_, filtered_, count, scratch_);
                    added = filtered_;
                }

                for (std::size_t index = 0U; index < count; ++index)
                {
                    const float value = added[index];
                    block[done + index] += value;
                    const float magnitude = value < 0.0f ? -value : value;

                    if (magnitude > stats_.component_peak)
                    {
                        stats_.component_peak = magnitude;
                    }

                    stats_.component_energy += static_cast<double>(value) * static_cast<double>(value);
                }

                stats_.component_frames += count;
            }

            done += count;
        }

        return frames;
    }

    [[nodiscard]] const StageStats& stats() const noexcept override
    {
        return stats_;
    }

private:
    void admit_events(std::uint64_t before) noexcept
    {
        while (!exhausted_ && pending_ < before)
        {
            if (active_count_ < max_active_events)
            {
                ActiveEvent& event = active_[active_count_++];
                event.start = pending_;
                event.ordinal = scheduled_ - 1U;
                event.envelope = 1.0;
                event.phase = pending_phase_;
                event.rng.reseed(seed_, 2U, index_ * 65536U + event.ordinal);
                stats_.events_applied++;
            }
            else
            {
                stats_.events_dropped++;
            }

            advance_pending();
        }
    }

    void advance_pending() noexcept
    {
        if (params_.poisson)
        {
            position_ += schedule_rng_.exponential(static_cast<double>(sample_rate_hz) / params_.rate_per_sec);
            const double rounded = det::round_half_even(position_);

            if (rounded < 0.0 || rounded >= static_cast<double>(end_))
            {
                exhausted_ = true;
                return;
            }

            pending_ = static_cast<std::uint64_t>(rounded);
        }
        else
        {
            if (period_ == 0U || next_periodic_ >= end_)
            {
                exhausted_ = true;
                return;
            }

            pending_ = next_periodic_;
            next_periodic_ += period_;
        }

        // Phases are drawn in schedule order so every event's phase is reproducible.
        pending_phase_ = params_.random_phase ? phase_rng_.uniform() * det::two_pi : params_.phase_degrees * (det::pi / 180.0);
        scheduled_++;
        stats_.events_scheduled = scheduled_;
    }

    ImpulseParams params_;
    double tau_{};
    std::uint64_t length_{};
    double decay_{};
    double amplitude_{};
    double radians_per_frame_{};
    std::uint64_t start_{};
    std::uint64_t end_{};
    std::uint64_t seed_{};
    std::uint32_t index_{};
    det::Pcg32 schedule_rng_{};
    det::Pcg32 phase_rng_{};
    double position_{};
    std::uint64_t next_periodic_{};
    std::uint64_t period_{};
    std::uint64_t pending_{};
    double pending_phase_{};
    std::uint32_t scheduled_{};
    bool exhausted_{};
    ActiveEvent active_[max_active_events] {};
    std::uint32_t active_count_{};
    BandFir fir_{};
    float excitation_[chunk_frames] {};
    float filtered_[chunk_frames] {};
    float scratch_[band_fir_history + chunk_frames] {};
    StageStats stats_{};
};

static_assert(alignof(ImpulseImpairment) <= impairment_alignment);

} // namespace

std::size_t impulse_size() noexcept
{
    return align_storage(sizeof(ImpulseImpairment));
}

Impairment* construct_impulse(const ImpulseParams& params, void* storage, std::size_t storage_bytes) noexcept
{
    if (storage == nullptr || reinterpret_cast<std::uintptr_t>(storage) % impairment_alignment != 0U || storage_bytes < impulse_size())
    {
        return nullptr;
    }

    return new (storage) ImpulseImpairment(params);
}

} // namespace signal_lab
