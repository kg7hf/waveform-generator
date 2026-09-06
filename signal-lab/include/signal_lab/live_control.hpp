#pragma once

// Dynamic, waveform-agnostic overlay after Engine. Every control is applied
// before one absolute OUTPUT frame; sample slips upstream therefore do not
// change the replay coordinate. A single producer owns this object. Queueing,
// processing, and reading its capture must not run concurrently.

#include "signal_lab/det_math.hpp"
#include "signal_lab/impairment.hpp"

#include <cstddef>
#include <cstdint>

namespace signal_lab
{

inline constexpr std::size_t live_pending_capacity = 64U;
inline constexpr std::size_t live_capture_capacity = 128U;
inline constexpr std::size_t live_sweep_capacity = 16U;
inline constexpr std::size_t live_static_capacity = 12U;

enum class ControlKind : std::uint8_t
{
    cw_enable,
    cw_frequency,
    cw_ci,
    static_enable,
    static_rate,
    static_peak,
    fade_now,
};

struct ControlEvent
{
    std::uint64_t frame{};
    ControlKind kind{ControlKind::cw_enable};
    double value{};                       // Enabled: 0 or 1; otherwise Hz, dB, or events/s.
    std::uint64_t duration_frames{};      // fade_now only; zero for every other kind.
};

struct ControlSweep
{
    std::uint64_t first_frame{};
    ControlKind kind{ControlKind::cw_frequency}; // cw_frequency, cw_ci, or fade_now.
    double values[live_sweep_capacity]{};
    std::size_t count{};
    std::uint64_t step_frames{};          // Positive; step i occurs at first_frame + i * step_frames.
    std::uint64_t fade_duration_frames{}; // fade_now only.
};

enum class ControlResult : std::uint8_t
{
    accepted,
    not_configured,
    invalid_event,
    late_event,
    pending_full,
    capture_full,
};

[[nodiscard]] const char* control_result_name(ControlResult result) noexcept;
[[nodiscard]] const char* control_kind_name(ControlKind kind) noexcept;

struct LiveState
{
    bool cw_enabled{};
    double cw_frequency_hz{1800.0};
    double cw_ci_db{3.0};
    bool static_enabled{};
    double static_rate_per_second{1.0};
    double static_peak_db{20.0};
    bool fade_active{};
    double fade_depth_db{};
    std::uint64_t fade_start_frame{};
    std::uint64_t fade_duration_frames{};
};

struct LiveStats
{
    std::uint64_t frames{};
    std::uint64_t output_digest{};          // FNV-1a of final emitted PCM16, including the clean disabled path.
    std::uint64_t clipped_samples{};
    std::uint64_t static_events_started{};
    std::uint64_t static_events_dropped{};
    std::uint32_t controls_accepted{};
    std::uint32_t controls_applied{};
    float peak{};                         // Quantized, saturated output amplitude.
    double output_energy{};
};

class LiveController
{
public:
    // reference_rms is the desired source's RMS after its configured source gain.
    // Reset disables all live impairments, resets the output timeline and RNGs,
    // and clears pending events and capture. An invalid reset leaves state intact.
    [[nodiscard]] bool reset(std::uint64_t seed, double reference_rms, const char** error = nullptr) noexcept;

    // Queueing never changes already produced audio. Equal-frame events execute
    // in enqueue order. No capture entries are discarded: a full capture rejects
    // further controls until reset. Preserve the capture externally before reset.
    [[nodiscard]] ControlResult enqueue(const ControlEvent& event) noexcept;
    // Expands atomically into ordinary captured events; failed sweeps enqueue none.
    // CW sweeps adjust parameters without implicitly enabling the CW component.
    [[nodiscard]] ControlResult enqueue_sweep(const ControlSweep& sweep) noexcept;

    // PCM16 in place, 0..engine_capacity_frames (2304) frames. Invalid calls consume
    // nothing. Processing allocates no memory and is independent of block size.
    // Signal path: source * live fade + live CW + live static -> PCM16 saturation.
    [[nodiscard]] bool process(std::int16_t* pcm, std::size_t frames, const char** error = nullptr) noexcept;

    [[nodiscard]] bool configured() const noexcept { return configured_; }
    [[nodiscard]] std::uint64_t frame() const noexcept { return stats_.frames; }
    [[nodiscard]] std::uint64_t seed() const noexcept { return seed_; }
    [[nodiscard]] double reference_rms() const noexcept { return reference_rms_; }
    [[nodiscard]] const LiveState& state() const noexcept { return state_; }
    [[nodiscard]] const LiveStats& stats() const noexcept { return stats_; }
    [[nodiscard]] std::size_t pending_count() const noexcept { return pending_count_; }
    [[nodiscard]] std::size_t capture_count() const noexcept { return capture_count_; }
    [[nodiscard]] const ControlEvent* capture_data() const noexcept { return capture_; }

private:
    struct StaticCrash
    {
        std::uint64_t end_frame{};
        double amplitude{};
        double envelope{};
        double phase{};
        bool active{};
    };

    [[nodiscard]] ControlResult validate(const ControlEvent& event) const noexcept;
    void insert(const ControlEvent& event) noexcept;
    void apply(const ControlEvent& event) noexcept;
    void schedule_static(std::uint64_t after_frame) noexcept;
    [[nodiscard]] double static_sample(std::uint64_t frame) noexcept;
    [[nodiscard]] double fade_gain(std::uint64_t frame) noexcept;

    bool configured_{};
    std::uint64_t seed_{};
    double reference_rms_{};
    LiveState state_{};
    LiveStats stats_{};
    ControlEvent capture_[live_capture_capacity]{};
    std::uint16_t pending_[live_pending_capacity]{}; // Indices into immutable captured events.
    std::size_t pending_count_{};
    std::size_t capture_count_{};
    double cw_phase_{};                     // Free-running even while CW is off; parameter changes retain it.
    double cw_step_{};
    double cw_amplitude_{};
    std::uint64_t next_static_frame_{unbounded_frames};
    double static_decay_{};
    det::Pcg32 static_schedule_rng_{};
    det::Pcg32 static_phase_rng_{};
    StaticCrash crashes_[live_static_capacity]{};
    det::StreamDigest digest_{};
};

} // namespace signal_lab
