#pragma once

#include "core/body_waveform.hpp"
#include "core/status.hpp"
#include "core/types.hpp"

#include <array>
#include <cstddef>
#include <span>

// =============================================================================
// synchronization.hpp - acquisition, delay-spread, timing recovery, carrier PLL
// =============================================================================
// This header declares the receiver's FRONT DOOR: the primitives that locate a
// MIL-STD-188-110B body burst in complex baseband and hand the rest of the
// receiver a locked reference (BodyAcquisition), plus the demodulation loops
// (root-raised-cosine matched filter, early/late symbol timing, and the
// decision-directed CarrierTracker PLL).
//
// Read this header as three groups:
//   1. Acquisition: BodyAcquisitionConfig / BodyAcquisition (+ diagnostics) and
//      acquire_body_preamble(), with the candidate-evaluation primitives a
//      streaming receiver can call one rolling position at a time
//      (make_body_prefix_reference, estimate_body_prefix_frequency_radians,
//      body_prefix_correlation, characterize_body_candidate,
//      body_candidate_is_valid).
//   2. Delay-spread: estimate_body_preamble_delay_spread() + its profile, used
//      to pick the equalizer geometry before training.
//   3. Demod loops: make_root_raised_cosine_taps(),
//      matched_filter_and_recover_timing(), CarrierTracker.
//
// The full teaching walkthrough - pipeline diagram, worked example with toy
// numbers, equations, fading-failure modes, and debugging signals - is in
//   core/synchronization-and-acquisition-explainer.md
// Implementation and governed-evidence rationale are in synchronization.cpp.
// =============================================================================

namespace m110
{

struct BodyAcquisitionConfig
{
    float symbol_rate_hz{2400.0F};
    float minimum_correlation{0.75F};
    float maximum_frequency_offset_hz{120.0F};
    std::size_t maximum_start_symbol{};
    // WBS 6.16 S2a/S2c (promoted to the default 2 Sep 2026; `false` restores
    // the pre-6.16 acquisition exactly). On a multipath channel every
    // propagation path produces its own consensus group, offset by the path
    // delay and all describing the same transmission; the pre-6.16 rule kept
    // the group with the most supporting segments (then correlation), which
    // on two equal-power paths is the ECHO in roughly a third of realizations
    // (measured: 34 % at 300L, 32 % at 150L, 36 % at 75L on the corrected
    // channel, carrying two thirds of the 300L error mass and all of the
    // 150L/75L error mass). The direct path then sits at a negative lag that
    // neither the delay-spread estimator (lags 0..15) nor the equalizer's
    // precursor span can reach, and the countdown decoded on that anchor can
    // be off by a segment or two. With this flag the winner is re-anchored to
    // the EARLIEST path of the same transmission (consensus group, then a
    // multi-segment negative-lag prefix search), the designators and
    // countdown are re-decoded on it, and the body boundary is the majority
    // over every preamble segment. Governed evidence: design M110-DD-009A
    // rev 3 §3-§5 (300L 4.36e-2 -> 4.30e-3 on the tuning block, held-out
    // block 3.84e-2 -> 4.03e-3; with the absolute collapse trigger 1.22e-5).
    bool anchor_earliest_path{true};
    std::size_t anchor_maximum_path_delay_symbols{15U};
    // Re-anchor only when the earlier path lies BEYOND the short geometry's
    // precursor reach (24 T/2 taps, cursor 12 = 6 symbols). Inside that reach
    // the shipped receiver already equalizes a precursor path (600L/1200L/2400L
    // at 2 ms = 5 symbols: zero errors on hundreds of "ds=0" trials), and
    // moving the anchor there only re-rolls a marginal DFE (measured: 2400L
    // 10 -> 77 errors over 200 seeds when re-anchoring at 5 symbols). The
    // 5 ms rows sit at 12 symbols, far beyond the reach.
    std::size_t anchor_minimum_path_delay_symbols{7U};
};

// What the earliest-path anchor step (WBS 6.16 S2) saw and decided, kept
// on the acquisition for the host's diagnostics: the precursor search's
// earliest significant lag and its power fraction of lag 0, the segments it
// accumulated, and how the re-characterization walk fared (segments tried
// and the count rejected by each test) - so a burst that keeps a later-path
// anchor can be traced to the test that kept it.
struct BodyAnchorDiagnostics
{
    // Sidelobe-corrected precursor power at lags 0..15 as a fraction of lag 0.
    std::array<float, 16U> precursor_powers{};
    // Qualifying peaks (earliest first) and the earliest one.
    std::size_t precursor_candidates{};
    std::size_t precursor_lag{};
    float precursor_fraction{};
    std::size_t precursor_segments{};
    std::size_t walk_segments{};
    std::size_t walk_rejected_frequency{};
    std::size_t walk_rejected_correlation{};
    std::size_t walk_rejected_invalid{};
    std::size_t walk_rejected_mode{};
    std::size_t reanchored_lag{};
    bool reanchored{};
};

struct BodyAcquisition
{
    std::size_t first_preamble_symbol{};
    std::size_t detected_segment_symbol{};
    std::size_t first_body_symbol{};
    std::uint8_t preamble_countdown{};
    bool preamble_start_in_buffer{};
    float symbol_rate_hz{static_cast<float>(body_symbol_rate_baud)};
    float frequency_offset_hz{};
    float carrier_phase_radians{};
    float normalized_correlation{};
    BodyModeRecognition recognition{};
    BodyAnchorDiagnostics anchor{};
};

[[nodiscard]] Result<BodyAcquisition> acquire_body_preamble(IQSampleSpan symbol_rate_input, const BodyAcquisitionConfig& config) noexcept;

// Candidate-evaluation primitives of acquire_body_preamble, exposed so a
// streaming receiver can evaluate one rolling candidate position per step
// instead of re-scanning a whole window every codec block. All operate on
// symbol-rate complex baseband and share the batch scanner's conventions.
//
// The 288-chip sync-scrambled acquisition-prefix reference; reference.size()
// must equal body_preamble_acquisition_prefix_symbols. Constant per build -
// compute once into persistent storage.
[[nodiscard]] Status make_body_prefix_reference(MutableIQSampleSpan reference) noexcept;
// Differential carrier estimate over the prefix at `start`, radians/symbol.
[[nodiscard]] float estimate_body_prefix_frequency_radians(IQSampleSpan input, std::size_t start, IQSampleSpan reference) noexcept;
// Normalized noncoherent prefix correlation in [0, 1]; carrier_phase_radians
// receives the coherent phase estimate at `start`.
[[nodiscard]] float body_prefix_correlation(IQSampleSpan input, std::size_t start, IQSampleSpan reference, float frequency_radians, float& carrier_phase_radians) noexcept;
// Decodes D1/D2/countdown at the seeded candidate (first_preamble_symbol,
// frequency_offset_hz, carrier_phase_radians, normalized_correlation filled
// in) and resolves mode recognition and the body boundary. The candidate
// start must have at least one whole 480-symbol segment of input ahead.
[[nodiscard]] BodyAcquisition characterize_body_candidate(IQSampleSpan input, BodyAcquisition candidate, const BodyAcquisitionConfig& config) noexcept;
// True when the characterized candidate names a supported mode with an
// internally consistent countdown.
[[nodiscard]] bool body_candidate_is_valid(const BodyAcquisition& candidate) noexcept;

// Estimates the channel delay spread in whole symbols at the acquired
// segment: the known acquisition prefix is correlated at symbol lags
// 0..maximum_delay_symbols, each lag scored as the noncoherent sum of its
// nine 32-chip block correlations so a fading path still registers, and the
// largest lag whose power reaches the significance fraction of the strongest
// path is returned (0 for a single-path channel). Streaming owners use this
// to select the equalizer geometry before training.
// Optional diagnostic view of the estimator's decision inputs (WBS 6.16 S0):
// the sidelobe-corrected noncoherent lag-power profile it thresholded, the
// strongest lag power, and how many preamble segments were accumulated. Pure
// copies of the estimator's own intermediates - requesting it never changes
// the returned delay spread.
struct BodyDelaySpreadProfile
{
    std::array<float, 16U> lag_powers{};
    float strongest_power{};
    std::size_t segments{};
};

[[nodiscard]] Result<std::size_t> estimate_body_preamble_delay_spread(IQSampleSpan symbol_rate_input, const BodyAcquisition& acquisition,
        std::size_t maximum_delay_symbols = 15U, BodyDelaySpreadProfile* profile = nullptr) noexcept;

struct TimingRecoveryConfig
{
    std::size_t samples_per_symbol{2U};
    std::size_t first_symbol_sample{};
    std::size_t phase_search_symbols{64U};
    float loop_gain{0.02F};
    float maximum_step_correction{0.25F};
};

struct TimingRecoveryProgress
{
    std::size_t symbols_written{};
    float final_sample_position{};
    float final_step_correction{};
    std::size_t selected_integer_phase{};
};

[[nodiscard]] Status make_root_raised_cosine_taps(std::size_t samples_per_symbol, float rolloff, std::span<float> taps) noexcept;
[[nodiscard]] Result<TimingRecoveryProgress> matched_filter_and_recover_timing(IQSampleSpan input, std::span<const float> matched_filter_taps, const TimingRecoveryConfig& config,
        MutableIQSampleSpan filtered_scratch, MutableIQSampleSpan recovered_symbols, MutableIQSampleSpan recovered_half_symbols = {}) noexcept;

class CarrierTracker
{
public:
    static constexpr float default_maximum_frequency_radians_per_symbol = 0.31415926535897932385F;

    [[nodiscard]] Status configure(float proportional_gain, float integral_gain,
                                   float maximum_frequency_radians_per_symbol = default_maximum_frequency_radians_per_symbol) noexcept;
    void reset(float phase_radians = 0.0F, float frequency_radians_per_symbol = 0.0F) noexcept;
    // Restore a previously observed (phase, frequency) verbatim, without the
    // wrap/clamp that reset() applies. The caller supplies values that are
    // already the tracker's own post-update output, so re-wrapping is skipped to
    // keep a restored state bit-identical to the state it was captured from.
    void restore(float phase_radians, float frequency_radians_per_symbol) noexcept;
    [[nodiscard]] IQSample update(IQSample input, IQSample decision) noexcept;
    void advance() noexcept;

    [[nodiscard]] float phase_radians() const noexcept
    {
        return phase_radians_;
    }

    [[nodiscard]] float frequency_radians_per_symbol() const noexcept
    {
        return frequency_radians_per_symbol_;
    }

    [[nodiscard]] float maximum_frequency_radians_per_symbol() const noexcept
    {
        return maximum_frequency_radians_per_symbol_;
    }

private:
    float proportional_gain_{};
    float integral_gain_{};
    float phase_radians_{};
    float frequency_radians_per_symbol_{};
    float maximum_frequency_radians_per_symbol_{default_maximum_frequency_radians_per_symbol};
};

} // namespace m110
