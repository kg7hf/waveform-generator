// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Paul R. Decker

#include "signal_lab/det_math.hpp"
#include "signal_lab/impairment.hpp"

#include <cmath>
#include <new>

namespace signal_lab
{

void resolve_window(const Window& window, std::uint64_t total_frames, std::uint64_t& start, std::uint64_t& end) noexcept
{
    start = det::seconds_to_frames(window.start_seconds, sample_rate_hz);

    if (window.has_duration)
    {
        end = start + det::seconds_to_frames(window.duration_seconds, sample_rate_hz);

        if (total_frames != 0U && end > total_frames)
        {
            end = total_frames;
        }
    }
    else
    {
        end = total_frames != 0U ? total_frames : unbounded_frames;
    }

    if (end < start)
    {
        end = start;
    }
}

namespace
{

class CwImpairment final : public Impairment
{
public:
    explicit CwImpairment(const CwParams& params) noexcept : params_
    {
        params
    } {}

    [[nodiscard]] StageType type() const noexcept override
    {
        return StageType::cw;
    }

    [[nodiscard]] bool prepare(const StageContext& context, const char** error) noexcept override
    {
        if (!(params_.frequency_hz > 0.0) || params_.frequency_hz >= static_cast<double>(sample_rate_hz) / 2.0)
        {
            if (error != nullptr)
            {
                *error = "cw.frequency_hz out of range";
            }

            return false;
        }

        resolve_window(params_.window, context.total_frames, start_, end_);
        length_ = end_ == unbounded_frames ? unbounded_frames : end_ - start_;
        ramp_ = det::seconds_to_frames(params_.ramp_seconds, sample_rate_hz);

        if (length_ != unbounded_frames && 2U * ramp_ > length_)
        {
            ramp_ = length_ / 2U;
        }

        phase_radians_ = params_.random_phase ? det::Pcg32(context.seed, 7U, context.index).uniform() * det::two_pi
                         : params_.phase_degrees * (det::pi / 180.0);
        amplitude_ = std::sqrt(2.0) * context.reference_rms * det::db_to_amplitude(-params_.ci_db);
        radians_per_frame_ = det::two_pi * params_.frequency_hz / static_cast<double>(sample_rate_hz);
        stats_ = StageStats{};
        stats_.events_scheduled = 1U;
        return true;
    }

    [[nodiscard]] std::size_t process(float* block, std::size_t frames, std::uint64_t first_input_frame, std::size_t) noexcept override
    {
        for (std::size_t index = 0U; index < frames; ++index)
        {
            const std::uint64_t absolute = first_input_frame + index;

            if (absolute < start_ || absolute >= end_)
            {
                continue;
            }

            const std::uint64_t t = absolute - start_;
            double envelope = 1.0;

            if (ramp_ != 0U)
            {
                std::uint64_t distance = t;

                if (length_ != unbounded_frames)
                {
                    const std::uint64_t tail = length_ - 1U - t;
                    distance = tail < distance ? tail : distance;
                }

                if (distance < ramp_)
                {
                    const double edge = det::sin(det::half_pi * (static_cast<double>(distance) / static_cast<double>(ramp_)));
                    envelope = edge * edge;
                }
            }

            const double angle = radians_per_frame_ * static_cast<double>(t) + phase_radians_;
            const auto value = static_cast<float>(amplitude_ * det::sin(angle) * envelope);
            block[index] += value;
            const float magnitude = value < 0.0f ? -value : value;

            if (magnitude > stats_.component_peak)
            {
                stats_.component_peak = magnitude;
            }

            stats_.component_energy += static_cast<double>(value) * static_cast<double>(value);
            stats_.component_frames++;

            if (t == 0U)
            {
                stats_.events_applied = 1U;
            }
        }

        return frames;
    }

    [[nodiscard]] const StageStats& stats() const noexcept override
    {
        return stats_;
    }

private:
    CwParams params_;
    std::uint64_t start_{};
    std::uint64_t end_{};
    std::uint64_t length_{};
    std::uint64_t ramp_{};
    double phase_radians_{};
    double amplitude_{};
    double radians_per_frame_{};
    StageStats stats_{};
};

static_assert(alignof(CwImpairment) <= impairment_alignment);

} // namespace

std::size_t cw_size() noexcept
{
    return align_storage(sizeof(CwImpairment));
}

Impairment* construct_cw(const CwParams& params, void* storage, std::size_t storage_bytes) noexcept
{
    if (storage == nullptr || reinterpret_cast<std::uintptr_t>(storage) % impairment_alignment != 0U || storage_bytes < cw_size())
    {
        return nullptr;
    }

    return new (storage) CwImpairment(params);
}

} // namespace signal_lab
