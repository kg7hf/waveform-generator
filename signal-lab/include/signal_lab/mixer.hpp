#pragma once

// The engine: PCM16 in -> gain -> ordered impairment stages -> saturate -> PCM16 out.
// One static object per platform; no allocation, no exceptions, block-size independent.

#include "signal_lab/det_math.hpp"
#include "signal_lab/impairment.hpp"
#include "signal_lab/scenario.hpp"

#include <cstddef>
#include <cstdint>

namespace signal_lab
{

struct EngineStats
{
    std::uint64_t frames_in{};
    std::uint64_t frames_out{};
    std::uint64_t clipped_samples{};
    std::uint64_t first_clipped_frame{0xFFFFFFFFFFFFFFFFULL};
    float peak{};
    double output_energy{};
    std::uint64_t output_digest{};   // FNV-1a 64 over the emitted PCM16 stream
    std::uint64_t source_digest{};   // FNV-1a 64 over the consumed PCM16 stream
    std::uint32_t stage_count{};
    std::uint32_t events_scheduled{};
    std::uint32_t events_applied{};
    std::uint32_t events_dropped{};
    std::size_t arena_bytes_used{};
};

class Engine
{
public:
    // reference_rms: RMS of the *unscaled* source over its reference interval.
    // total_frames: source length in frames, or 0 when unknown (live sources).
    [[nodiscard]] bool configure(const Scenario& scenario, double reference_rms, std::uint64_t total_frames, const char** error) noexcept;

    // Pass-through configuration (no stages, unity gain); useful when no scenario is present.
    void configure_passthrough() noexcept;

    [[nodiscard]] bool configured() const noexcept
    {
        return configured_;
    }

    // frames <= engine_block_frames; output capacity >= frames + engine_slack_frames.
    // Returns the number of output frames written (differs from `frames` only with sample slips).
    [[nodiscard]] std::size_t process(const std::int16_t* input, std::size_t frames, std::int16_t* output, std::size_t output_capacity) noexcept;

    [[nodiscard]] const EngineStats& stats() const noexcept
    {
        return stats_;
    }

    // The scenario passed to configure() must outlive the engine's use of it (no copy is kept).
    [[nodiscard]] const Scenario& scenario() const noexcept
    {
        return *scenario_;
    }

    [[nodiscard]] const Impairment* stage(std::uint32_t index) const noexcept
    {
        return index < stage_count_ ? stages_[index] : nullptr;
    }

    [[nodiscard]] double scaled_reference_rms() const noexcept
    {
        return scaled_reference_;
    }

private:
    const Scenario* scenario_{};
    std::uint32_t stage_count_{};
    Impairment* stages_[max_stages]{};
    alignas(impairment_alignment) unsigned char arena_[impairment_arena_bytes]{};
    float work_[engine_capacity_frames]{};
    float scratch_[engine_capacity_frames]{};
    double input_scale_{1.0 / 32768.0};
    double scaled_reference_{};
    std::uint64_t cursor_{};
    bool configured_{};
    bool saturate_{true};
    EngineStats stats_{};
    det::StreamDigest output_digest_{};
    det::StreamDigest source_digest_{};
};

} // namespace signal_lab
