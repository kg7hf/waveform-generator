// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

#pragma once

// Portable scenario description: the same JSON document drives the Phase 1
// Python renderer (host/signal_lab), the Phase 2 host renderer and the Phase 3
// RT1170 player. Grammar (signal-lab.scenario/1):
//
// {
//   "schema": "signal-lab.scenario/1", "test_id": "...", "seed": 42,
//   "source": {"path": "..."},                      (ignored by the engine)
//   "reference": "auto" | {"start_seconds": s, "duration_seconds": d},
//   "reference_rms": 0.38,                          (Phase 2: pre-measured level for streaming targets)
//   "source_gain_db": -12, "clip_policy": "saturate" | "reject",
//   "impairments": [{"type": "watterson"|"frequency_shift"|"second_source"|"fade"|"awgn"|"cw"|"impulse"|"sample_slip", "stream_id": 1..4294967295, ...}]
// }
//
// "watterson" (the multipath fading channel; docs/modules/signal-lab-watterson.md) is at
// most one stage, the first: {"type": "watterson", "paths": [{"delay_ms", "power_db",
// "fading": "rayleigh"|"fixed", "spread_hz", "shift_hz", "phase_deg"}, ...]}, 1 to 4 paths.
// "cw" requires exactly one of frequency_hz or frequency_sweep (with low_hz,
// high_hz and slope_hz_per_second). It optionally pairs pulse_period_seconds
// with pulse_duty_cycle; both period and on-time resolve to whole frames. An
// optional markov_on_off object supplies mean_on_seconds and mean_off_seconds;
// it may combine with either frequency form, but not with periodic pulse fields.
// An optional fsk object supplies baud, full mark-space separation shift_hz,
// and pattern; it requires fixed frequency_hz rather than frequency_sweep.
// An optional fading object supplies spread_hz for complex Rayleigh fading of
// the CW component only.
// "second_source" uses source_path, si_db, optional start_frame,
// source_start_frame, duration_frames and frequency_offset_hz; it is one
// bounded overlay after the primary channel/shift and before other stages.
// Its optional fading object supplies spread_hz for an independent unit-power
// Rayleigh process. Fading and frequency offset share one analytic filter.
// Optional markov_on_off gives mean_on_seconds and mean_off_seconds; it gates
// the filtered contribution in destination-window time, without another delay.

#include "common/status.hpp"
#include "signal_lab/cw_fading.hpp"
#include "signal_lab/cw_fsk.hpp"
#include "signal_lab/cw_sweep.hpp"
#include "signal_lab/frequency_shift_params.hpp"
#include "signal_lab/markov_gate.hpp"
#include "signal_lab/noise_bandwidth.hpp"
#include "signal_lab/second_source_params.hpp"
#include "signal_lab/stream_rate.hpp"
#include "signal_lab/units/decibel.hpp"
#include "signal_lab/watterson_params.hpp"

#include <cstddef>
#include <cstdint>

namespace signal_lab
{

// The engine's rate in hertz for the legacy stages, the scenario ranges and the hosts:
// stream_rate (stream_rate.hpp) is the one definition, so every stage scales together.
inline constexpr std::uint32_t sample_rate_hz = stream_rate.hertz();
inline constexpr std::uint32_t max_stages = 9U;
inline constexpr std::uint32_t max_events = 32U;
inline constexpr std::uint32_t max_test_id = 96U;
inline constexpr std::size_t second_source_path_capacity = 256U;
// Maximum deleted or duplicated run retained by the portable streaming engine.
inline constexpr std::uint32_t max_slip_length = 1024U;
inline constexpr std::uint64_t max_cw_pulse_frames = 48000000000000ULL; // 1e9 seconds at the 48 kHz stream rate.

enum class StageType : std::uint8_t
{
    awgn,
    cw,
    impulse,
    fade,
    sample_slip,
    watterson,
    frequency_shift,
    second_source,
};

struct Window
{
    double start_seconds{0.0};
    bool has_duration{false};
    double duration_seconds{0.0};
};

struct AwgnParams
{
    units::Ratio snr{}; // by N0 x 3000 Hz in 300-3300 Hz (awgn.hpp; E.5.5)
    Window window{};
};

struct CwParams
{
    // Zero for a swept-frequency descriptor; otherwise the fixed tone frequency.
    double frequency_hz{};
    double ci_db{};
    bool random_phase{false};
    double phase_degrees{0.0};
    double ramp_seconds{0.01};
    Window window{};
    // Zero/zero preserves the legacy single-window tone. Otherwise, pulse
    // starts repeat every period; on-time is measured in exact sample frames.
    std::uint64_t pulse_period_frames{};
    std::uint64_t pulse_on_frames{};
    // When enabled, the tone starts at low and rises with configured slope.
    CwSweepParams sweep{};
    // When enabled, discrete geometric ON/OFF dwells replace a periodic pulse schedule.
    MarkovGateParams markov{};
    // When enabled, frequency_hz is the FSK center frequency.
    FskParams fsk{};
    // When enabled, complex Rayleigh fading applies to this CW component only.
    CwFadingParams fading{};
};

struct ImpulseParams
{
    double peak_db{};
    double decay_ms{};
    bool ring_noise{false};
    double ring_hz{1700.0};
    double truncate_tau{8.0};
    bool random_phase{true};
    double phase_degrees{0.0};
    bool poisson{true};
    double rate_per_sec{1.0};
    double period_seconds{1.0};
    bool has_first{false};
    double first_seconds{0.0};
    Window window{};
};

enum class FadeSchedule : std::uint8_t
{
    explicit_starts,
    periodic,
    poisson,
};

struct FadeParams
{
    double depth_db{};
    bool rectangular{false};
    bool explicit_shape{false};
    double attack_ms{};
    double hold_ms{};
    double recovery_ms{};
    double duration_ms{};
    FadeSchedule schedule{FadeSchedule::periodic};
    double starts_seconds[max_events]{};
    std::uint32_t start_count{};
    bool has_first{false};
    double first_seconds{0.0};
    double period_seconds{10.0};
    bool has_count{false};
    std::uint32_t count{};
    double rate_per_sec{};
    Window window{};
};

struct SlipEvent
{
    double at_seconds{};
    bool duplicate{false};
    std::uint32_t length_samples{};
};

struct SampleSlipParams
{
    SlipEvent events[max_events]{};
    std::uint32_t event_count{};
};

struct Stage
{
    StageType type{StageType::awgn};
    // Optional JSON identity for random-stream derivation; zero means omitted.
    std::uint32_t stream_id{};
    AwgnParams awgn{};
    CwParams cw{};
    ImpulseParams impulse{};
    FadeParams fade{};
    SampleSlipParams slip{};
};

struct Scenario
{
    char test_id[max_test_id]{};
    std::uint64_t seed{};
    double source_gain_db{-12.0};
    bool saturate{true};
    bool reference_auto{true};
    NoiseBandwidth noise_bandwidth{NoiseBandwidth::khz3};
    double reference_start_seconds{};
    double reference_duration_seconds{};
    bool has_reference_rms{false};
    double reference_rms{};
    Stage stages[max_stages]{};
    std::uint32_t stage_count{};
    // One slot, not a member of each Stage: at most one Watterson stage, the first.
    WattersonParams watterson{};
    // One whole-signal shifter, after Watterson (if present), before noise.
    FrequencyShiftParams frequency_shift{};
    // One borrowed-file source descriptor. The host/target adapter owns the
    // source; this path is its external identity, not a DSP filesystem handle.
    SecondSourceParams second_source{};
    char second_source_path[second_source_path_capacity]{};
};

// Parse a scenario document into `scenario`: success, or why the document is
// refused, a Status with a static message (invalid_argument for the grammar,
// invalid_configuration for validate_scenario()'s rules, busy while another
// parse holds the reader). The document must fit the fixed-capacity JSON reader.
[[nodiscard]] m110::Status parse_scenario(const char* text, std::size_t length, Scenario& scenario) noexcept;

// Validate both parsed and programmatically constructed scenarios before any
// array indexing or conversion of time/level parameters to integer frames:
// success, or invalid_configuration with a static message saying which rule.
[[nodiscard]] m110::Status validate_scenario(const Scenario& scenario) noexcept;

[[nodiscard]] const char* stage_type_name(StageType type) noexcept;

} // namespace signal_lab
