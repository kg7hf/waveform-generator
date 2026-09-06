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
//   "impairments": [{"type": "fade"|"awgn"|"cw"|"impulse"|"sample_slip", ...}]
// }

#include <cstddef>
#include <cstdint>

namespace signal_lab
{

inline constexpr std::uint32_t sample_rate_hz = 48000U;
inline constexpr std::uint32_t max_stages = 8U;
inline constexpr std::uint32_t max_events = 32U;
inline constexpr std::uint32_t max_test_id = 96U;
// Maximum deleted or duplicated run retained by the portable streaming engine.
inline constexpr std::uint32_t max_slip_length = 1024U;

enum class StageType : std::uint8_t
{
    awgn,
    cw,
    impulse,
    fade,
    sample_slip,
};

struct Window
{
    double start_seconds{0.0};
    bool has_duration{false};
    double duration_seconds{0.0};
};

struct AwgnParams
{
    double snr_db{};
    Window window{};
};

struct CwParams
{
    double frequency_hz{};
    double ci_db{};
    bool random_phase{false};
    double phase_degrees{0.0};
    double ramp_seconds{0.01};
    Window window{};
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
    double reference_start_seconds{};
    double reference_duration_seconds{};
    bool has_reference_rms{false};
    double reference_rms{};
    Stage stages[max_stages]{};
    std::uint32_t stage_count{};
};

// Parse a scenario document. On failure returns false and sets *error to a
// static message. The document must fit the fixed-capacity JSON reader.
[[nodiscard]] bool parse_scenario(const char* text, std::size_t length, Scenario& scenario, const char** error) noexcept;

[[nodiscard]] const char* stage_type_name(StageType type) noexcept;

} // namespace signal_lab
