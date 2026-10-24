// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// Unit conversions that need det_math (the linear forms of decibels) or a
// rounding rule (frames and phase steps).
// Guide: docs/modules/signal-lab-watterson.md, "Types and units".

#include "signal_lab/convert.hpp"
#include "signal_lab/det_math.hpp"
#include "signal_lab/units/conversions.hpp"
#include "signal_lab/units/decibel.hpp"

#include <cmath>
#include <numbers>

namespace signal_lab::units
{
namespace
{

constexpr const char* delay_message = "units::to_frames: the delay must be finite and >= 0, at a non-zero rate, below 2^53 frames";
constexpr const char* shift_message = "units::phase_step: the shift must be finite and within +/-fs / 2, at a non-zero rate";
// fs / 2: the largest shift a phase step represents.
constexpr double nyquist_fraction = 0.5;

[[nodiscard]] m110::Status refused(const char* message) noexcept
{
    return m110::Status{m110::StatusCode::invalid_argument, message};
}

// The decibel: 10 log10 for a power ratio, 20 log10 for an amplitude ratio.
constexpr double power_decibels_per_decade = 10.0;
constexpr double amplitude_decibels_per_decade = 20.0;

[[nodiscard]] double power_from_db(double decibels) noexcept
{
    return det::exp(decibels * (std::numbers::ln10 / power_decibels_per_decade));
}

[[nodiscard]] double amplitude_from_db(double decibels) noexcept
{
    return det::exp(decibels * (std::numbers::ln10 / amplitude_decibels_per_decade));
}

} // namespace

double power_ratio(Gain gain) noexcept
{
    return power_from_db(gain.in(dB));
}

double power_ratio(Ratio ratio) noexcept
{
    return power_from_db(ratio.in(dB));
}

double amplitude_ratio(Gain gain) noexcept
{
    return amplitude_from_db(gain.in(dB));
}

double amplitude_ratio(Ratio ratio) noexcept
{
    return amplitude_from_db(ratio.in(dB));
}

Gain gain_from_amplitude(double amplitude) noexcept
{
    return Gain::from_db(amplitude_decibels_per_decade * det::log(amplitude) / std::numbers::ln10);
}

m110::Result<SampleCount> to_frames(Duration delay, SampleRate rate) noexcept
{
    const double milliseconds = delay.in(ms);

    if (!std::isfinite(milliseconds) || milliseconds < 0.0 || rate.hertz() == 0U)
    {
        return refused(delay_message);
    }

    const double frames = det::round_half_even(milliseconds * as_double(rate.hertz()) / milliseconds_per_second);

    if (!(frames < exact_frame_limit))
    {
        return refused(delay_message);
    }

    return SampleCount{whole_to_uint64(frames)};
}

Duration to_duration(SampleCount frames, SampleRate rate) noexcept
{
    return Duration{as_double(frames.value()) * milliseconds_per_second / as_double(rate.hertz()), ms};
}

m110::Result<PhaseStep> phase_step(FrequencyDelta shift, SampleRate rate) noexcept
{
    const double hertz = shift.in(Hz);
    const double rate_hertz = as_double(rate.hertz());

    // |f| <= fs / 2 bounds the increment to [-2^31, 2^31], inside the int64 the cast forms.
    if (!std::isfinite(hertz) || rate.hertz() == 0U || std::abs(hertz) > nyquist_fraction * rate_hertz)
    {
        return refused(shift_message);
    }

    const double increment = det::round_half_even(hertz * phase_turn / rate_hertz);
    return PhaseStep{twos_complement(whole_to_int64(increment))};
}

FrequencyDelta realised_shift(PhaseStep step, SampleRate rate) noexcept
{
    return FrequencyDelta{as_double(as_signed(step.word())) * as_double(rate.hertz()) / phase_turn, Hz};
}

} // namespace signal_lab::units
