// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

#include "signal_lab/cw_sweep.hpp"

#include "signal_lab/det_math.hpp"

#include <cmath>

namespace signal_lab
{
namespace
{

constexpr double phase_turn = 18446744073709551616.0;
constexpr double exact_integer_limit = 9007199254740992.0;
constexpr std::uint64_t half_turn_word = std::uint64_t{1U} << 63U;
constexpr const char* invalid_message = "cw.frequency_sweep: bounds must be in [1, 23999] Hz and slope must be positive/representable";

} // namespace

std::uint64_t cw_sweep_cycle_step(const CwSweepParams& params, units::SampleRate rate) noexcept
{
    const double span = params.high.in(units::Hz)-params.low.in(units::Hz);
    const double slope = params.slope.hertz_per_second();

    if (rate.hertz() == 0U || !std::isfinite(span) || !(span > 0.0) || !std::isfinite(slope) || !(slope > 0.0))
    {
        return 0U;
    }

    const double cycles_per_frame = slope / (2.0 * span * static_cast<double>(rate.hertz()));
    const double ideal = cycles_per_frame * phase_turn;

    if (!std::isfinite(ideal) || ideal < 0.5 || ideal >= exact_integer_limit)
    {
        return 0U;
    }

    return static_cast<std::uint64_t>(det::round_half_even(ideal));
}

units::Frequency cw_sweep_frequency_at_phase(const CwSweepParams& params, std::uint64_t phase_word) noexcept
{
    const double low = params.low.in(units::Hz);
    const double span = params.high.in(units::Hz)-low;
    const double phase = static_cast<double>(phase_word) / phase_turn;
    const double hertz = phase < 0.5 ? low + 2.0 * span * phase : low + 2.0 * span * (1.0 - phase);
    return units::Frequency::from_hz(hertz);
}

double interval_average_cw_frequency(const CwSweepParams& params, std::uint64_t phase_word, std::uint64_t phase_step) noexcept
{
    const double here = cw_sweep_frequency_at_phase(params, phase_word).in(units::Hz);

    if (phase_step == 0U)
    {
        return here;
    }

    const std::uint64_t available = phase_word < half_turn_word ? half_turn_word - phase_word : UINT64_MAX - phase_word + 1U;
    const std::uint64_t next_phase = phase_word + phase_step;
    const double next = cw_sweep_frequency_at_phase(params, next_phase).in(units::Hz);

    if (phase_step <= available)
    {
        return 0.5 * (here + next);
    }

    const double corner = phase_word < half_turn_word ? params.high.in(units::Hz) : params.low.in(units::Hz);
    const double before_fraction = static_cast<double>(available) / static_cast<double>(phase_step);
    return before_fraction * (0.5 * (here + corner)) + (1.0 - before_fraction) * (0.5 * (corner + next));
}

m110::Status validate_cw_sweep_params(const CwSweepParams& params) noexcept
{
    if (!params.enabled)
    {
        return m110::Status::success();
    }

    const double low = params.low.in(units::Hz);
    const double high = params.high.in(units::Hz);
    const double slope = params.slope.hertz_per_second();

    if (!std::isfinite(low) || !std::isfinite(high) || low < 1.0 || high > 23999.0 || !(high > low) || !std::isfinite(slope) || !(slope > 0.0) || cw_sweep_cycle_step(params, stream_rate) == 0U)
    {
        return m110::Status{m110::StatusCode::invalid_configuration, invalid_message};
    }

    return m110::Status::success();
}

units::Frequency instantaneous_cw_frequency(const CwSweepParams& params, units::SampleIndex frame, units::SampleRate rate) noexcept
{
    const std::uint64_t step = cw_sweep_cycle_step(params, rate);
    return cw_sweep_frequency_at_phase(params, frame.value() * step);
}

} // namespace signal_lab
