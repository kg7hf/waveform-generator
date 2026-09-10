#pragma once

// The engine: normalized float in -> gain -> ordered impairment stages -> clip
// -> normalized float out. PCM16 remains as an explicit legacy adapter.

#include "signal_lab/det_math.hpp"
#include "signal_lab/impairment.hpp"
#include "signal_lab/scenario.hpp"

#include <cstddef>
#include <cstdint>

namespace signal_lab
{

enum class ProcessError : std::uint8_t
{
    none,
    invalid_buffer,
    clipping,
};

struct EngineStats
{
    std::uint64_t frames_in{};
    std::uint64_t frames_out{};
    std::uint64_t clipped_samples{};
    std::uint64_t first_clipped_frame{0xFFFFFFFFFFFFFFFFULL};
    float peak{};                 // Absolute peak in normalized full-scale units.
    double output_energy{};       // Sum of squares in normalized full-scale units.
    std::uint64_t output_digest{};   // FNV-1a 64 over the emitted canonical PCM stream
    std::uint64_t source_digest{};   // FNV-1a 64 over the consumed canonical PCM stream
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
    // On failure returns zero and latches process_error() until reconfiguration. A
    // rejected block consumes its input but commits no output samples or output stats.
    [[nodiscard]] std::size_t process(const std::int16_t* input, std::size_t frames, std::int16_t* output, std::size_t output_capacity) noexcept;

    // Production path. Inputs and outputs are normalized [-1, 1), and remain
    // float between the source, static engine, and live controller. Statistics
    // and digests describe the eventual signed PCM24 quantization.
    [[nodiscard]] std::size_t process(const float* input, std::size_t frames,
                                      float* output, std::size_t output_capacity) noexcept;

    [[nodiscard]] ProcessError process_error() const noexcept
    {
        return process_error_;
    }

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
    double source_gain_{1.0};
    double scaled_reference_{};
    std::uint64_t cursor_{};
    bool configured_{};
    bool saturate_{true};
    ProcessError process_error_{ProcessError::none};
    EngineStats stats_{};
    det::StreamDigest output_digest_{};
    det::StreamDigest source_digest_{};
};

} // namespace signal_lab
