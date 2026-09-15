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

class AwgnImpairment final : public Impairment
{
public:
    explicit AwgnImpairment(const AwgnParams& params) noexcept : params_
    {
        params
    } {}

    [[nodiscard]] StageType type() const noexcept override
    {
        return StageType::awgn;
    }

    [[nodiscard]] bool prepare(const StageContext& context, const char** error) noexcept override
    {
        if (params_.snr_db < -120.0 || params_.snr_db > 120.0)
        {
            if (error != nullptr)
            {
                *error = "awgn.snr_db out of range";
            }

            return false;
        }

        resolve_window(params_.window, context.total_frames, start_, end_);
        scale_ = static_cast<float>(context.reference_rms * det::db_to_amplitude(-params_.snr_db));
        rng_.reseed(context.seed, 1U, context.index);
        fir_.reset(&rng_, true);
        stats_ = StageStats{};
        stats_.events_scheduled = 1U;
        return true;
    }

    [[nodiscard]] std::size_t process(float* block, std::size_t frames, std::uint64_t first_input_frame, std::size_t) noexcept override
    {
        std::size_t done = 0U;

        while (done < frames)
        {
            const std::size_t count = (frames - done) < chunk_frames ? (frames - done) : chunk_frames;

            for (std::size_t index = 0U; index < count; ++index)
            {
                excitation_[index] = static_cast<float>(rng_.gaussian());
            }

            fir_.filter(excitation_, noise_, count, scratch_);

            for (std::size_t index = 0U; index < count; ++index)
            {
                const std::uint64_t absolute = first_input_frame + done + index;

                if (absolute < start_ || absolute >= end_)
                {
                    continue;
                }

                const float value = noise_[index] * scale_;
                block[done + index] += value;
                const float magnitude = value < 0.0f ? -value : value;

                if (magnitude > stats_.component_peak)
                {
                    stats_.component_peak = magnitude;
                }

                stats_.component_energy += static_cast<double>(value) * static_cast<double>(value);
                stats_.component_frames++;
            }

            done += count;
        }

        stats_.events_applied = 1U;
        return frames;
    }

    [[nodiscard]] const StageStats& stats() const noexcept override
    {
        return stats_;
    }

private:
    static constexpr std::size_t chunk_frames = 256U;

    AwgnParams params_;
    std::uint64_t start_{};
    std::uint64_t end_{};
    float scale_{};
    det::Pcg32 rng_{};
    BandFir fir_{};
    float excitation_[chunk_frames] {};
    float noise_[chunk_frames] {};
    float scratch_[band_fir_history + chunk_frames] {};
    StageStats stats_{};
};

static_assert(alignof(AwgnImpairment) <= impairment_alignment);

} // namespace

std::size_t awgn_size() noexcept
{
    return align_storage(sizeof(AwgnImpairment));
}

Impairment* construct_awgn(const AwgnParams& params, void* storage, std::size_t storage_bytes) noexcept
{
    if (storage == nullptr || reinterpret_cast<std::uintptr_t>(storage) % impairment_alignment != 0U || storage_bytes < awgn_size())
    {
        return nullptr;
    }

    return new (storage) AwgnImpairment(params);
}

} // namespace signal_lab
