// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

#pragma once

#include "signal_lab/stage_resources.hpp"

// The Watterson multipath fading channel (MIL-STD-188-110D Appendix E; ITU-R
// F.1487 / F.520): up to 4 paths, each with an integer delay, a relative level,
// Rayleigh fading with a Gaussian Doppler spread or a fixed gain, and a shift.
// Guide: docs/modules/signal-lab-watterson.md; design docs/design/watterson-stage.md.
//
//   y(n) = Re{ sum_i g_i(n) xa(n - L_i) },  g_i(n) = a_i h_i(n) r_i(n)
//
// xa is the analytic input (analytic_filter.hpp), h_i the interpolated Gaussian tap
// of a Rayleigh path (doppler_filter.hpp) or 1 for a fixed path, r_i the path's
// rotator exp(j (2 pi f_i n / fs + phi_i)), and a_i the normalisation that makes the
// path powers sum to 1 (E.5.4). The output is a function of (seed, stage, path,
// absolute frame) only, so any partition into blocks gives the same samples.

#include "common/status.hpp"
#include "signal_lab/analytic_filter.hpp"
#include "signal_lab/complex_sample.hpp"
#include "signal_lab/doppler_filter.hpp"
#include "signal_lab/impairment.hpp"
#include "signal_lab/units/conversions.hpp"
#include "signal_lab/units/decibel.hpp"
#include "signal_lab/units/frequency.hpp"
#include "signal_lab/units/sampling.hpp"
#include "signal_lab/watterson_params.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

namespace signal_lab
{

// The PCG32 family of the Watterson tap processes: no other stage draws from it
// (in use: 1 AWGN, 2/4/5 impulse, 7 CW, 8 fade, 0x4C53000x live; design 8).
inline constexpr std::uint32_t watterson_rng_family = 9U;
// Path i of stage s draws from stream 4 s + i.
inline constexpr std::uint32_t watterson_streams_per_stage = watterson_max_paths;
// The rotator is set exactly from the integer phase at every absolute frame with
// n mod 1024 = 0 and stepped in between; 1023 steps in double stay below 1e-12 (design 4.5).
inline constexpr std::uint64_t rotator_resync_frames = 1024U;
static_assert((rotator_resync_frames & (rotator_resync_frames - 1U)) == 0U, "a power of two, so the resync is a mask");
// The identity of the stage's arithmetic: part of every parameter digest, so a change
// of arithmetic also changes the identity. Raise it with any change of output bits.
inline constexpr std::uint64_t watterson_algorithm_version = 1U;

// Bounds on the static state (design 11.2): 20 KiB leaves room for 111 Doppler taps (Q1).
inline constexpr std::size_t watterson_channel_budget_bytes = 20480U;

// Every message is static. The configuration messages are the schema's (design 9.3).
[[nodiscard]] m110::Status validate_watterson(const WattersonParams& params) noexcept;

// The realised parameters of one path.
struct WattersonPathReport
{
    PathFading fading{PathFading::rayleigh};
    units::SampleCount delay{};           // L_i, rounded to the nearest frame (E.5.2)
    units::SampleCount frames_per_node{}; // M_i (0 for a fixed path)
    units::FrequencyDelta node_rate{};    // R_i (0 for a fixed path)
    double rate_multiple{};               // K_i = R_i / d_i, a pure ratio
    std::uint32_t doppler_taps{};         // N_i, a count of coefficients (0 for a fixed path)
    double amplitude{};                   // a_i, the linear normalisation (dimensionless)
    double interpolation_power{};         // v_i (1 for a fixed path)
    units::Gain gain{};                   // 20 log10 a_i
    units::PhaseStep phase_step{};        // the shift as a 32-bit phase increment
    units::FrequencyDelta realised_shift{};
    units::Angle phase{}; // phi_i (fixed paths)
};

struct WattersonReport
{
    std::array<WattersonPathReport, watterson_max_paths> paths{};
    std::uint32_t path_count{};
    std::uint32_t hilbert_taps{};
    units::SampleCount latency{};     // D
    std::uint64_t parameter_digest{}; // FNV-1a over the version, rate, seed, stage and realised parameters
};

// Channel truth (survey gap C10): one record per node per Rayleigh path, and one per
// fixed path at configure time. With the report's M, a_i, phase steps and delays the
// 48 kHz path gains are rebuilt exactly (design 9.7).
struct TapRecord
{
    units::SampleIndex node_frame{};
    std::uint32_t path{};
    ComplexFloat value{};
};

// Host diagnostics only: the board passes no observer and pays nothing.
class TapObserver
{
public:
    virtual void on_tap(const TapRecord& record) noexcept = 0;

protected:
    TapObserver() = default;
    ~TapObserver() = default; // C.35: protected and non-virtual; never deleted through this interface
    TapObserver(const TapObserver&) = default;
    TapObserver& operator=(const TapObserver&) = default;
    TapObserver(TapObserver&&) = default;
    TapObserver& operator=(TapObserver&&) = default;
};

// The state of one path. Every member is written by configure_fixed(),
// configure_rayleigh() or clear(). The configure functions return the refusal of a
// unit conversion or the Doppler design (unreachable for validated parameters) as a value.
class WattersonPathState
{
public:
    struct Setup
    {
        const WattersonPath* path{};
        double power{};         // p_i, the normalised power
        std::uint32_t index{};  // i
        det::Pcg32 generator{}; // the path's own stream
        TapObserver* observer{};
    };

    void clear() noexcept;
    [[nodiscard]] m110::Status configure_fixed(const Setup& setup, WattersonPathReport& report) noexcept;
    [[nodiscard]] m110::Status configure_rayleigh(const Setup& setup, WattersonPathReport& report) noexcept;

    // Returns the configured path gain for one consecutive frame. Call only after a
    // successful configure_* and in increasing frame order beginning at frame zero;
    // this advances Rayleigh node state. The observer is optional.
    [[nodiscard]] ComplexFloat next_gain(units::SampleIndex frame, TapObserver* observer = nullptr) noexcept;

    // Adds (or, for the first path, writes) this path's term for each frame of the chunk:
    // output[k] (+)= Re g(n) xI(n - L) - Im g(n) xQ(n - L), n = first + k.
    void render(std::span<float> output, units::SampleIndex first, const AnalyticRing& ring, bool accumulate, TapObserver* observer) noexcept;

private:
    [[nodiscard]] ComplexDouble exact_rotator(units::SampleIndex frame) const noexcept;
    void advance_node(TapObserver* observer) noexcept;
    [[nodiscard]] m110::Status configure_rotator(const WattersonPath& path, double phase_radians, WattersonPathReport& report) noexcept;

    DopplerFilter mDoppler{};
    ComplexFloat mNode{};           // h[m], scaled by a_i
    ComplexFloat mDelta{};          // h[m + 1] - h[m]
    ComplexFloat mNextNode{};       // h[m + 1]
    ComplexFloat mFixedGain{};      // a_i e^(j phi_i) without a shift; a_i with one
    ComplexDouble mRotator{};       // r(n) for the next frame
    ComplexDouble mRotatorStep{};   // exp(j 2 pi inc / 2^32)
    double mInverseFramesPerNode{}; // 1 / M_i
    double mPhaseOffset{};          // phi_i in radians, for a rotating fixed path
    std::uint64_t mNodeIndex{};     // m
    units::SampleCount mDelay{};
    std::uint32_t mFramesPerNode{}; // M_i (0 for a fixed path)
    std::uint32_t mNodeOffset{};    // j = n - m M_i
    std::uint32_t mPhaseIncrement{};
    std::uint32_t mIndex{};
    PathFading mFading{PathFading::fixed};
    bool mRotates{};
};

// The bulk state of the stage, about 17.5 KiB: trivially copyable, no virtual
// functions, static storage that the platform places (OCRAM2 .ocram.watterson,
// NOLOAD, on the board) and the engine borrows. configure() writes every member
// before process() reads it, so the object never depends on startup initialisation.
class WattersonChannel
{
public:
    // Validates the parameters, designs the filters and draws each Rayleigh path's
    // first N + 1 samples. The observer is borrowed; it may be null.
    [[nodiscard]] m110::Status configure(const WattersonParams& params, std::uint64_t seed, std::uint32_t stage_index, TapObserver* observer) noexcept;

    // Processes the block in place. first_frame must be the frame after the previous
    // block (0 after configure()); otherwise the channel latches a fault and every
    // later call fails until configure().
    [[nodiscard]] m110::Status process(std::span<float> block, units::SampleIndex first_frame) noexcept;

    [[nodiscard]] const WattersonReport& report() const noexcept { return mReport; }

    // The Hilbert coefficients in use, for reference models.
    [[nodiscard]] std::span<const float, hilbert_pairs> hilbert_coefficients() const noexcept { return mAnalytic.coefficients(); }

private:
    void process_chunk(std::span<float> chunk, units::SampleIndex first) noexcept;
    void finish_report(const WattersonParams& params, std::uint64_t seed, std::uint32_t stage_index) noexcept;

    AnalyticFilter mAnalytic{};
    AnalyticRing mRing{};
    std::array<WattersonPathState, watterson_max_paths> mPaths{};
    std::uint32_t mPathCount{};
    units::SampleIndex mNextFrame{};
    TapObserver* mObserver{};
    WattersonReport mReport{};
    bool mConfigured{};
    bool mFault{};
};

static_assert(std::is_trivially_copyable_v<WattersonChannel>, "static, NOLOAD-placeable state");
static_assert(sizeof(WattersonChannel) <= watterson_channel_budget_bytes, "design 11.2");

// The Impairment adapter of the stage, built by the engine in its typed slot. It
// borrows the scenario's parameters, the channel and the observer; it owns nothing.
class WattersonStage final : public Impairment
{
public:
    WattersonStage(const WattersonParams& params, WattersonChannel* channel, TapObserver* observer) noexcept : mParams{&params}, mChannel{channel}, mObserver{observer} {}

    [[nodiscard]] StageType type() const noexcept override { return StageType::watterson; }

    // Configures the channel for the scenario's seed and this stage's index: the
    // channel's Status, or invalid_configuration without a channel.
    [[nodiscard]] m110::Status prepare(const StageContext& context) noexcept override;

    // The legacy pointer-and-count boundary of Impairment, viewed once as a span: the
    // block's frames, or the channel's fault as its Status (the engine latches it); the
    // block then holds no output.
    [[nodiscard]] m110::Result<std::size_t> process(float* block, std::size_t frames, std::uint64_t first_input_frame, std::size_t capacity) noexcept override;

    [[nodiscard]] const StageStats& stats() const noexcept override { return mStats; }

    // The realised parameters; nullptr without a channel.
    [[nodiscard]] const WattersonReport* report() const noexcept { return mChannel == nullptr ? nullptr : &mChannel->report(); }

private:
    const WattersonParams* mParams{};
    WattersonChannel* mChannel{};
    TapObserver* mObserver{};
    StageStats mStats{};
};

} // namespace signal_lab
