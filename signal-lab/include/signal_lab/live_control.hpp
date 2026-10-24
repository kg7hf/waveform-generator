// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

#pragma once

// Dynamic, waveform-agnostic overlay after Engine. Every control is applied
// before one absolute OUTPUT frame; sample slips upstream therefore do not
// change the replay coordinate. A single producer owns this object. Queueing,
// processing, and reading its capture must not run concurrently.

#include "common/status.hpp"
#include "signal_lab/det_math.hpp"
#include "signal_lab/impairment.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace signal_lab
{

inline constexpr std::size_t live_pending_capacity = 64U;
inline constexpr std::size_t live_capture_capacity = 128U;
inline constexpr std::size_t live_sweep_capacity = 16U;
inline constexpr std::size_t live_static_capacity = 12U;
inline constexpr std::size_t live_cw_capacity = 4U;

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
    double value{};                  // Enabled: 0 or 1; otherwise Hz, dB, or events/s.
    std::uint64_t duration_frames{}; // fade_now only; zero for every other kind.
    std::uint32_t oscillator{};      // CW slot; zero for non-CW controls.
};

struct ControlSweep
{
    std::uint64_t first_frame{};
    ControlKind kind{ControlKind::cw_frequency}; // cw_frequency, cw_ci, or fade_now.
    double values[live_sweep_capacity]{};
    std::size_t count{};
    std::uint64_t step_frames{};          // Positive; step i occurs at first_frame + i * step_frames.
    std::uint64_t fade_duration_frames{}; // fade_now only.
    std::uint32_t oscillator{};           // CW slot; zero for fade sweeps.
};

// Why the controller refused a control (enqueue(), enqueue_sweep()). Each
// message is the code a refused live control's reply carries:
// rt1170/error_map.hpp publishes them (published_control_refusals).
namespace control_refusal
{

// No successful reset() yet.
inline constexpr m110::Status not_configured{.code = m110::StatusCode::unavailable, .message = "not_configured"};
// A value, slot, duration or kind the control cannot have, or a sweep that
// does not fit the timeline.
inline constexpr m110::Status invalid_event{.code = m110::StatusCode::invalid_argument, .message = "invalid_event"};
// A frame the controller has already produced.
inline constexpr m110::Status late_event{.code = m110::StatusCode::timeout, .message = "late_event"};
// live_pending_capacity controls already wait for their frames.
inline constexpr m110::Status pending_full{.code = m110::StatusCode::busy, .message = "pending_full"};
// The capture holds live_capture_capacity controls until the next reset().
inline constexpr m110::Status capture_full{.code = m110::StatusCode::buffer_too_small, .message = "capture_full"};

inline constexpr std::array all{not_configured, invalid_event, late_event, pending_full, capture_full};

} // namespace control_refusal

[[nodiscard]] const char* control_kind_name(ControlKind kind) noexcept;

struct LiveOscillatorState
{
    bool enabled{};
    double frequency_hz{1800.0};
    double ci_db{3.0};
};

struct LiveState
{
    LiveOscillatorState cw[live_cw_capacity]{};
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
    std::uint64_t output_digest{}; // FNV-1a of final emitted PCM16, including the clean disabled path.
    std::uint64_t clipped_samples{};
    std::uint64_t static_events_started{};
    std::uint64_t static_events_dropped{};
    std::uint32_t controls_accepted{};
    std::uint32_t controls_applied{};
    float peak{}; // Quantized, saturated output amplitude.
    double output_energy{};
};

class LiveController
{
public:
    // reference_rms is the desired source's RMS after its configured source gain.
    // Reset disables all live impairments, resets the output timeline and RNGs,
    // and clears pending events and capture: success, or invalid_argument for a
    // reference RMS outside (0, 32768], which leaves state intact.
    [[nodiscard]] m110::Status reset(std::uint64_t seed, double reference_rms) noexcept;

    // Queueing never changes already produced audio. Equal-frame events execute
    // in enqueue order. No capture entries are discarded: a full capture rejects
    // further controls until reset. Preserve the capture externally before reset.
    // Success, or a control_refusal, which changes nothing.
    [[nodiscard]] m110::Status enqueue(const ControlEvent& event) noexcept;
    // Expands atomically into ordinary captured events; failed sweeps enqueue none.
    // CW sweeps adjust parameters without implicitly enabling the CW component.
    [[nodiscard]] m110::Status enqueue_sweep(const ControlSweep& sweep) noexcept;

    // PCM16 legacy adapter, 0..engine_capacity_frames (2304) frames. Invalid calls consume
    // nothing. Processing allocates no memory and is independent of block size.
    // Signal path: source * live fade + live CW bank + live static -> PCM16 saturation.
    // Success, or why the block was refused: unavailable before a successful
    // reset(), invalid_argument for a missing block, more than
    // engine_capacity_frames or an output timeline that would overflow.
    [[nodiscard]] m110::Status process(std::int16_t* pcm, std::size_t frames) noexcept;

    // Production normalized-float path. The block remains float so static and
    // live processing do not introduce an intermediate quantizer. Statistics
    // and digest describe the final packed-PCM24 boundary. The refusals are the
    // PCM16 path's, and invalid_argument for a non-finite sample.
    [[nodiscard]] m110::Status process(float* pcm, std::size_t frames) noexcept;

    [[nodiscard]] m110::Status process(std::nullptr_t, std::size_t frames) noexcept { return process(static_cast<float*>(nullptr), frames); }

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

    [[nodiscard]] m110::Status validate(const ControlEvent& event) const noexcept;
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
    double cw_phase_[live_cw_capacity]{}; // Free-running even while CW is off; parameter changes retain it.
    double cw_step_[live_cw_capacity]{};
    double cw_amplitude_[live_cw_capacity]{};
    std::uint64_t next_static_frame_{unbounded_frames};
    double static_decay_{};
    det::Pcg32 static_schedule_rng_{};
    det::Pcg32 static_phase_rng_{};
    StaticCrash crashes_[live_static_capacity]{};
    det::StreamDigest digest_{};
};

} // namespace signal_lab
