// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

#pragma once

// The AWGN stage at the lane's default SNR convention: N0 x 3000 Hz in 300-3300 Hz
// (MIL-STD-188-110D E.5.5 refers SNR to the input power; lane criterion 1).
// Guide: docs/modules/signal-lab-engine.md, "AWGN"; design docs/design/watterson-stage.md 7.
//
//   e(n)   unit-variance white Gaussian excitation: Box-Muller pairs, first to frame 2k,
//          second to frame 2k + 1 (det_gaussian.hpp); E.5.3, white to fs / 2. A second
//          value serves only its own frame, once: an odd frame without it (a block
//          after a sample slip changed the count) draws a fresh pair, so no value repeats
//   h      a Kaiser band (A = 60 dB, cutoffs 150 and 3450 Hz, 583 taps), unit gain at
//          1800 Hz, flat within +/-0.01 dB over 300-3300 Hz
//   noise  sigma_w (h * e)(n),  sigma_w = R 10^(-SNR / 20) sqrt(fs / (2 x 3000 Hz))
//
// The one-sided density at 1800 Hz is N0 = 2 sigma_w^2 / fs, so N0 x 3000 Hz is
// R^2 10^(-SNR / 10) exactly: the delivered SNR by the lane's convention is the
// label, with no offset. The total noise is N0 x ENBW, ENBW = 3233.4 Hz.
// Scenario-wide NoiseBandwidth selects 3/6/9/12 kHz. For a wider band, replace
// 3000 above by BW, the high edge by 300 + BW and the centre by 300 + BW/2;
// the tap count, transitions, input-power reference and operation order stay fixed.
//
// The stage is built in one of the engine's typed slots (mixer.hpp), not in the
// placement arena: no cast and no placement new. Its working buffer is the
// engine's per-call scratch; its FIR history and pair carry are its own.

#include "signal_lab/det_math.hpp"
#include "signal_lab/impairment.hpp"
#include "signal_lab/kaiser.hpp"
#include "signal_lab/noise_bandwidth.hpp"
#include "signal_lab/scenario.hpp"
#include "signal_lab/stream_rate.hpp"
#include "signal_lab/units/amplitude.hpp"
#include "signal_lab/units/decibel.hpp"
#include "signal_lab/units/frequency.hpp"
#include "signal_lab/units/sampling.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string_view>

namespace signal_lab
{

// The lane's SNR band, 300-3300 Hz (110D 5.3.2.5, 3 kHz), and its 3000 Hz reference
// bandwidth: N0 x 3000 Hz (E.5.5; lane criterion 1).
inline constexpr units::Frequency noise_band_low{300.0, units::Hz};
inline constexpr units::Frequency noise_band_high{3300.0, units::Hz};
inline constexpr units::FrequencyDelta snr_reference_bandwidth = noise_band_high - noise_band_low;
// The kernel's transitions, 300 Hz wide and centred 150 Hz outside the band edges,
// and its stopband (design 7.3): flat to +/-0.01 dB inside the band.
inline constexpr units::FrequencyDelta noise_band_transition{300.0, units::Hz};
inline constexpr units::Ratio noise_band_attenuation{60.0, units::dB};
inline constexpr std::string_view noise_band_design_method{"kaiser-sinc"};
static_assert(kaiser::within_design_range(noise_band_attenuation), "Kaiser's A > 50 dB beta formula and the I0 series bound (beta <= 10)");
// The band centre, E.7.1's reference frequency: the kernel has unit gain there.
inline constexpr units::Frequency noise_band_reference{1800.0, units::Hz};

// The Kaiser length rounded up to odd, so the kernel is symmetric about one centre tap.
[[nodiscard]] constexpr std::uint32_t noise_band_length(units::SampleRate rate) noexcept
{
    const std::uint32_t kaiser_taps = kaiser::length(noise_band_attenuation, noise_band_transition, rate);
    return kaiser_taps | 1U;
}

inline constexpr std::uint32_t noise_band_taps = noise_band_length(stream_rate);
inline constexpr std::uint32_t noise_band_history = noise_band_taps - 1U;
inline constexpr std::uint32_t noise_band_centre = noise_band_history / 2U;
static_assert(noise_band_taps % 2U == 1U, "symmetric about one centre tap");

// At most two AWGN stages per scenario: representative effect chains use one; one spare (design 7.3, Q9).
inline constexpr std::uint32_t max_awgn_stages = 2U;
// The PCG32 family of the AWGN excitation, unchanged (design 8).
inline constexpr std::uint32_t awgn_rng_family = 1U;
// The SNR range the scenario accepts, unchanged.
inline constexpr units::Ratio awgn_max_snr{120.0, units::dB};

// The excitation of each chunk follows the FIR history in the engine's scratch.
static_assert(engine_capacity_frames > noise_band_history, "larger AWGN bandwidths at higher sample rates need a new chunk strategy");
inline constexpr std::size_t awgn_chunk_frames = engine_capacity_frames - noise_band_history;
static_assert(awgn_chunk_frames > 0U, "the engine's scratch holds the history and a chunk");

// The shared kernel: h[0] (an end tap) ... h[centre], computed once with det_math.
struct NoiseBandKernel
{
    std::array<float, noise_band_centre + 1U> half{};
    units::FrequencyDelta enbw{}; // (fs / 2) sum h^2 of this stored kernel
};

// Designs the 3 kHz kernel on first use (configure time, never on the audio path)
// and returns it. Not reentrant: configure engines from one task, as the player does.
// On the RT1170 player the shared kernel bank lies in OCRAM2 (.ocram.noise_band_kernel).
[[nodiscard]] const NoiseBandKernel& noise_band_kernel() noexcept;
// Selects one shared kernel. Invalid enum values are refused without indexing the bank.
[[nodiscard]] m110::Result<std::reference_wrapper<const NoiseBandKernel>> noise_band_kernel(NoiseBandwidth bandwidth) noexcept;

// sigma_w for a reference RMS and an SNR by N0 x 3000 Hz.
[[nodiscard]] units::Amplitude awgn_noise_sigma(units::Amplitude reference, units::Ratio snr) noexcept;
// sigma_w for N0 x bandwidth; an invalid enum returns invalid_configuration.
[[nodiscard]] m110::Result<units::Amplitude> awgn_noise_sigma(units::Amplitude reference, units::Ratio snr, NoiseBandwidth bandwidth) noexcept;

class AwgnStage final : public Impairment
{
public:
    explicit AwgnStage(const AwgnParams& params) noexcept : mParams{params} {}

    [[nodiscard]] StageType type() const noexcept override { return StageType::awgn; }

    [[nodiscard]] m110::Status prepare(const StageContext& context) noexcept override;

    // The legacy pointer-and-count boundary of Impairment; viewed once as a span.
    [[nodiscard]] m110::Result<std::size_t> process(float* block, std::size_t frames, std::uint64_t first_input_frame, std::size_t capacity) noexcept override;

    [[nodiscard]] const StageStats& stats() const noexcept override { return mStats; }

private:
    [[nodiscard]] float excitation(units::SampleIndex frame) noexcept;
    void add_chunk(std::span<float> output, units::SampleIndex first) noexcept;

    AwgnParams mParams{};
    std::array<float, noise_band_history> mHistory{};
    std::span<float> mScratch{};
    const NoiseBandKernel* mKernel{};
    det::Pcg32 mGenerator{};
    units::SampleIndex mStart{}; // the window [mStart, mEnd) in absolute frames
    units::SampleIndex mEnd{};
    units::Amplitude mSigma{};        // sigma_w
    float mSampleScale{};             // sigma_w as the float the sample path multiplies by
    float mCarry{};                   // the second value of the last pair ...
    units::SampleIndex mCarryFrame{}; // ... and the frame it belongs to
    bool mCarryPending{};             // not yet used by that frame
    StageStats mStats{};
};

} // namespace signal_lab
