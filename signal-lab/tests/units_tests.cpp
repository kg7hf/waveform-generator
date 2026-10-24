// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// Catch2 suite of signal-lab's unit types (signal_lab/units/*.hpp) and their
// conversions: design 9.1, tests U1, U2 and U35.
// Guide: docs/modules/signal-lab-watterson.md, "Types and units".

#include "signal_lab/stream_rate.hpp"
#include "signal_lab/units/angle.hpp"
#include "signal_lab/units/conversions.hpp"
#include "signal_lab/units/decibel.hpp"
#include "signal_lab/units/duration.hpp"
#include "signal_lab/units/frequency.hpp"
#include "signal_lab/units/sampling.hpp"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numbers>
#include <type_traits>

namespace
{

namespace units = signal_lab::units;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

// ---- compile-time negative checks (U35): an expression that means nothing does not compile ----

template <class First, class Second>
concept addable = requires(First first, Second second) { first + second; };

template <class First, class Second>
concept subtractable = requires(First first, Second second) { first - second; };

template <class Value>
concept negatable = requires(Value value) { -value; };

template <class Value>
concept scalable = requires(Value value, double scale) { value* scale; };

static_assert(!addable<units::Level, units::Level>, "two absolute levels do not add");
static_assert(!negatable<units::Level>, "an absolute level does not negate");
static_assert(!addable<units::Gain, units::Ratio> && !addable<units::Ratio, units::Gain>, "a path level plus an SNR means nothing (QA #20)");
static_assert(!subtractable<units::Gain, units::Ratio>, "nor does their difference");
static_assert(addable<units::Level, units::Gain> && addable<units::Level, units::Ratio> && addable<units::Gain, units::Gain>, "the defined algebra");
static_assert(std::is_same_v<decltype(units::Level{} - units::Level{}), units::Ratio>, "two levels differ by a ratio");
static_assert(!addable<units::Frequency, units::Frequency>, "two points on the dial do not add");
static_assert(!scalable<units::Frequency> && !negatable<units::Frequency>, "a point does not scale or negate");
static_assert(scalable<units::FrequencyDelta> && negatable<units::FrequencyDelta>, "a difference does");
static_assert(std::is_same_v<decltype(units::Frequency{} - units::Frequency{}), units::FrequencyDelta>, "two points differ by a delta");
static_assert(std::is_same_v<decltype(units::FrequencyDelta{} / units::FrequencyDelta{}), double>, "a ratio of deltas is a number");
static_assert(!addable<units::SampleIndex, units::SampleIndex>, "two stream positions do not add");
static_assert(std::is_same_v<decltype(units::SampleIndex{} - units::SampleIndex{}), units::SampleCount>, "positions differ by a count");
static_assert(!std::is_constructible_v<units::FrequencyDelta, double>, "no frequency without a unit");
static_assert(!std::is_constructible_v<units::Duration, double>, "no duration without a unit");
static_assert(!std::is_constructible_v<units::Gain, double> && !std::is_constructible_v<units::Ratio, double>, "no decibels without a unit");
static_assert(!std::is_convertible_v<units::Gain, units::Ratio> && !std::is_convertible_v<units::Ratio, units::Gain>, "gain and ratio stay apart");

// ---- values ----

// 1.8 kHz is 1800 Hz: the E.7.1 reference frequency.
constexpr double reference_khz = 1.8;
constexpr double reference_hz = 1800.0;
// The lane's band: 300-3300 Hz, 3000 Hz wide.
constexpr double band_low_hz = 300.0;
constexpr double band_high_hz = 3300.0;
constexpr double band_width_hz = 3000.0;
// 2 ms = 0.002 s = 2000 us = 96 frames at 48 kHz (CCIR Poor's second path).
constexpr double delay_ms = 2.0;
constexpr double delay_s = 0.002;
constexpr double delay_us = 2000.0;
constexpr std::uint64_t delay_frames = 96U;
constexpr std::uint64_t later_frame = 1000U;
// A half turn and a quarter turn.
constexpr double half_turn_degrees = 180.0;
constexpr double quarter_turn_degrees = 90.0;
constexpr double quarter_turn_radians = std::numbers::pi / 2.0;
// Two roundings of a double product: a few ulp.
constexpr double angle_bound = 1.0e-15;

// 10^(-3/10), the power ratio of -3 dB, and 10^(20/20) = 10.
constexpr double minus_three_db = -3.0;
constexpr double minus_three_db_power = 0.50118723362727224;
constexpr double twenty_db = 20.0;
constexpr double twenty_db_amplitude = 10.0;
constexpr double round_trip_db = -7.5;
// A decibel conversion through det::exp and det::log: a few 1e-14 relative.
constexpr double decibel_bound = 1.0e-12;
// Levels and the gain and ratio between them.
constexpr double reference_dbfs = -12.0;
constexpr double noise_dbfs = -30.0;
constexpr double reference_to_noise_db = 18.0;
constexpr double attenuated_dbfs = -15.0;

// U1: delays in ms and the frames E.5.2 rounds them to at 48 kHz (design 4.2 and 10.1).
struct DelayCase
{
    double milliseconds;
    std::uint64_t frames;
};

constexpr std::array delay_cases{
    DelayCase{0.0, 0U},
    DelayCase{0.5, 24U},
    DelayCase{2.0, 96U},
    DelayCase{4.5, 216U},
    DelayCase{9.0, 432U},
    DelayCase{20.0, 960U},
    // Ties to even: 1/32 ms is 1.5 frames, 3/32 ms is 4.5 frames, 193/96 ms is 96.5 frames.
    DelayCase{0.03125, 2U},
    DelayCase{0.09375, 4U},
    DelayCase{193.0 / 96.0, 96U},
};

// U2: the phase step is fs / 2^32; a realised shift is within half a step, 5.6 uHz.
constexpr double phase_resolution_hz = 48000.0 / 4294967296.0;
constexpr double half_phase_step_hz = phase_resolution_hz / 2.0;
constexpr std::array shift_cases{0.0, 0.01, -0.01, 75.0, -75.0, 1.0e-6};
constexpr double largest_shift_hz = 75.0;

// The refusals of the conversions: values a validated scenario never holds.
constexpr double infinity = std::numeric_limits<double>::infinity();
constexpr double not_a_number = std::numeric_limits<double>::quiet_NaN();
constexpr std::array refused_delays_ms{-1.0, -1.0e-9, infinity, -infinity, not_a_number, 1.0e300};
constexpr double nyquist_hz = 24000.0;
constexpr double beyond_nyquist_hz = 24000.5;
constexpr std::array refused_shifts_hz{beyond_nyquist_hz, -beyond_nyquist_hz, infinity, -infinity, not_a_number, 1.0e300};

} // namespace

TEST_CASE("units: frequencies, durations and angles round-trip through their units", "[units]")
{
    CHECK(units::Frequency{reference_khz, units::kHz}.in(units::Hz) == reference_hz);
    CHECK(units::FrequencyDelta{reference_hz, units::Hz}.in(units::kHz) == reference_khz);
    CHECK((units::Frequency{band_high_hz, units::Hz} - units::Frequency{band_low_hz, units::Hz}) == units::FrequencyDelta{band_width_hz, units::Hz});
    CHECK(units::Duration{delay_ms, units::ms}.in(units::us) == delay_us);
    CHECK(units::Duration{delay_s, units::s}.in(units::ms) == delay_ms);
    CHECK(units::to_frames(units::Duration{delay_ms, units::ms}, signal_lab::stream_rate).value() == units::SampleCount{delay_frames});
    CHECK(units::to_duration(units::SampleCount{delay_frames}, signal_lab::stream_rate) == units::Duration{delay_ms, units::ms});
    CHECK_THAT((units::Angle{half_turn_degrees, units::degrees}.in(units::radians)), WithinRel(std::numbers::pi, angle_bound));
    CHECK_THAT((units::Angle{quarter_turn_radians, units::radians}.in(units::degrees)), WithinRel(quarter_turn_degrees, angle_bound));
    CHECK((units::SampleIndex{later_frame} - units::SampleIndex{later_frame - delay_frames}) == units::SampleCount{delay_frames});
}

TEST_CASE("units: decibel types convert to linear power and amplitude ratios", "[units]")
{
    CHECK_THAT((units::power_ratio(units::Gain{minus_three_db, units::dB})), WithinRel(minus_three_db_power, decibel_bound));
    CHECK_THAT((units::amplitude_ratio(units::Ratio{twenty_db, units::dB})), WithinRel(twenty_db_amplitude, decibel_bound));
    CHECK(units::power_ratio(units::Gain{0.0, units::dB}) == 1.0);
    CHECK_THAT((units::gain_from_amplitude(units::amplitude_ratio(units::Gain{round_trip_db, units::dB})).in(units::dB)), WithinAbs(round_trip_db, decibel_bound));
    CHECK((units::Level{reference_dbfs, units::dBFS} - units::Level{noise_dbfs, units::dBFS}) == units::Ratio{reference_to_noise_db, units::dB});
    CHECK((units::Level{reference_dbfs, units::dBFS} + units::Gain{minus_three_db, units::dB}) == units::Level{attenuated_dbfs, units::dBFS});
}

TEST_CASE("units: delays round to the nearest frame, ties to even (U1, E.5.2)", "[units][watterson]")
{
    for (const DelayCase& delay : delay_cases)
    {
        INFO("delay " << delay.milliseconds << " ms");
        CHECK(units::to_frames(units::Duration{delay.milliseconds, units::ms}, signal_lab::stream_rate).value().value() == delay.frames);
    }
}

TEST_CASE("units: shifts become 32-bit phase steps within 5.6 uHz (U2)", "[units][watterson]")
{
    for (const double shift : shift_cases)
    {
        INFO("shift " << shift << " Hz");
        const units::PhaseStep step = units::phase_step(units::FrequencyDelta{shift, units::Hz}, signal_lab::stream_rate).value();
        const double realised = units::realised_shift(step, signal_lab::stream_rate).in(units::Hz);
        CHECK(std::abs(realised - shift) <= half_phase_step_hz);
    }

    const std::uint32_t up = units::phase_step(units::FrequencyDelta{largest_shift_hz, units::Hz}, signal_lab::stream_rate).value().word();
    const std::uint32_t down = units::phase_step(units::FrequencyDelta{-largest_shift_hz, units::Hz}, signal_lab::stream_rate).value().word();
    CHECK(units::phase_step(units::FrequencyDelta{}, signal_lab::stream_rate).value().word() == 0U);
    CHECK(down == 0U - up);
}

TEST_CASE("units: a delay or shift out of a conversion's domain is refused as a value, never cast", "[units][watterson]")
{
    for (const double delay : refused_delays_ms)
    {
        INFO("delay " << delay << " ms");
        const m110::Result<units::SampleCount> frames = units::to_frames(units::Duration{delay, units::ms}, signal_lab::stream_rate);
        CHECK_FALSE(frames.has_value());
        CHECK(frames.status().code == m110::StatusCode::invalid_argument);
    }

    for (const double shift : refused_shifts_hz)
    {
        INFO("shift " << shift << " Hz");
        CHECK_FALSE(units::phase_step(units::FrequencyDelta{shift, units::Hz}, signal_lab::stream_rate).has_value());
    }

    CHECK_FALSE(units::to_frames(units::Duration{delay_ms, units::ms}, units::SampleRate{}).has_value());
    CHECK_FALSE(units::phase_step(units::FrequencyDelta{}, units::SampleRate{}).has_value());
    // The edges of the domains convert: fs / 2 is the step 2^31 in either sign.
    CHECK(units::phase_step(units::FrequencyDelta{nyquist_hz, units::Hz}, signal_lab::stream_rate).value().word() == 0x80000000U);
    CHECK(units::phase_step(units::FrequencyDelta{-nyquist_hz, units::Hz}, signal_lab::stream_rate).value().word() == 0x80000000U);
    CHECK(units::to_frames(units::Duration{}, signal_lab::stream_rate).value() == units::SampleCount{});
}
