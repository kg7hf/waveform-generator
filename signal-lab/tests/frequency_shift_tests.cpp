// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// Catch2 suite for the global offset and triangle Doppler sweep.

#include "signal_lab/frequency_shift.hpp"

#include "signal_lab/det_math.hpp"
#include "test_spectrum.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <utility>
#include <vector>

namespace
{

namespace sl = signal_lab;
namespace units = signal_lab::units;

constexpr std::uint32_t rate_hz = 48000U;
constexpr double frequency_tolerance_hz = 0.1;

[[nodiscard]] double measured_frequency(const std::vector<double>& positive_crossings, double center_frame)
{
    constexpr std::size_t cycles = 50U;
    auto upper = std::lower_bound(positive_crossings.begin(), positive_crossings.end(), center_frame);
    REQUIRE(upper != positive_crossings.begin());
    REQUIRE(upper != positive_crossings.end());
    std::size_t center = static_cast<std::size_t>(upper - positive_crossings.begin());

    if (std::abs(positive_crossings[center - 1U] - center_frame) < std::abs(positive_crossings[center] - center_frame))
    {
        --center;
    }

    REQUIRE(center >= cycles / 2U);
    REQUIRE(center + cycles / 2U < positive_crossings.size());
    const double elapsed_frames = positive_crossings[center + cycles / 2U] - positive_crossings[center - cycles / 2U];
    return static_cast<double>(cycles) * static_cast<double>(rate_hz) / elapsed_frames;
}

TEST_CASE("frequency shift: parameters fail closed and offset/sweep domains are explicit", "[frequency_shift]")
{
    sl::FrequencyShiftParams params{};
    CHECK(sl::validate_frequency_shift_params(params).is_ok());
    params.offset = units::FrequencyDelta{75.0, units::Hz};
    CHECK(sl::validate_frequency_shift_params(params).is_ok());
    params.offset = units::FrequencyDelta{150.0, units::Hz};
    CHECK(sl::validate_frequency_shift_params(params).is_ok());
    params.offset = units::FrequencyDelta{-150.0001, units::Hz};
    CHECK_FALSE(sl::validate_frequency_shift_params(params).is_ok());
    params.offset = units::FrequencyDelta{std::numeric_limits<double>::infinity(), units::Hz};
    CHECK_FALSE(sl::validate_frequency_shift_params(params).is_ok());
    params = sl::FrequencyShiftParams{};
    params.mode = sl::FrequencyShiftMode::sweep;
    CHECK(sl::validate_frequency_shift_params(params).is_ok());
    params.start_frequency = units::FrequencyDelta{0.0, units::Hz};
    CHECK(sl::validate_frequency_shift_params(params).is_ok());
    CHECK(sl::instantaneous_frequency(params, units::SampleIndex{0U}).in(units::Hz) == 0.0);
    params.start_frequency = units::FrequencyDelta{75.1, units::Hz};
    CHECK_FALSE(sl::validate_frequency_shift_params(params).is_ok());
    params.start_frequency = units::FrequencyDelta{-75.0, units::Hz};
    params.direction = sl::TriangleDirection::descending;
    CHECK(sl::validate_frequency_shift_params(params).is_ok());
    params.direction = sl::TriangleDirection::ascending;
    params.slope = sl::FrequencySlope{0.0};
    CHECK_FALSE(sl::validate_frequency_shift_params(params).is_ok());
    params.slope = sl::FrequencySlope{std::numeric_limits<double>::quiet_NaN()};
    CHECK_FALSE(sl::validate_frequency_shift_params(params).is_ok());
    params.slope = sl::FrequencySlope{10000000.0};
    CHECK_FALSE(sl::validate_frequency_shift_params(params).is_ok());
}

TEST_CASE("frequency shift: triangle truth is absolute-indexed, periodic and continuous at turns", "[frequency_shift]")
{
    sl::FrequencyShiftParams params{};
    params.mode = sl::FrequencyShiftMode::sweep;
    params.slope = sl::FrequencySlope{3.5};
    const auto frequency = [&](std::uint64_t frame) { return sl::instantaneous_frequency(params, units::SampleIndex{frame}).in(units::Hz); };

    CHECK(frequency(0U) == -75.0);
    CHECK(std::abs(frequency(2057143U) - 75.0) <= frequency_tolerance_hz);
    CHECK(std::abs(frequency(4114286U) + 75.0) <= frequency_tolerance_hz);
    const double measured_slope = (frequency(1000000U) - frequency(999000U)) * static_cast<double>(rate_hz) / 1000.0;
    CHECK(std::abs(measured_slope - 3.5) < 1.0e-6);
    constexpr std::uint64_t distant = (std::uint64_t{1U} << 32U) + 12345U;
    CHECK(std::abs(frequency(distant) - (-50.5988229167)) < 1.0e-5);
    CHECK(std::isfinite(sl::instantaneous_frequency(params, units::SampleIndex{std::numeric_limits<std::uint64_t>::max()}).in(units::Hz)));
    const units::SampleRate other_rate{96000U};
    constexpr std::uint64_t other_half_frames = 4114286U;
    CHECK(std::abs(sl::instantaneous_frequency(params, units::SampleIndex{other_half_frames}, other_rate).in(units::Hz)-75.0) <= frequency_tolerance_hz);
    params.start_frequency = units::FrequencyDelta{0.0, units::Hz};
    const std::uint64_t first_turn = 1028572U;
    CHECK(sl::validate_frequency_shift_params(params).is_ok());
    CHECK(frequency(0U) == 0.0);
    CHECK(std::abs(frequency(first_turn) - 75.0) < frequency_tolerance_hz);
    CHECK(std::abs(frequency(first_turn + 1U) - 75.0) < frequency_tolerance_hz);
    params.direction = sl::TriangleDirection::descending;
    CHECK(sl::validate_frequency_shift_params(params).is_ok());
    CHECK(frequency(0U) == 0.0);
    CHECK(frequency(1U) < 0.0);
    CHECK(std::abs(frequency(first_turn) + 75.0) < frequency_tolerance_hz);
}

TEST_CASE("frequency shift: default sweep CW measurements cover the noninteger full period", "[frequency_shift]")
{
    sl::FrequencyShiftParams params{};
    params.mode = sl::FrequencyShiftMode::sweep;
    sl::FrequencyShiftStage stage{params};
    REQUIRE(stage.prepare(sl::StageContext{}).is_ok());
    constexpr double source_hz = 1000.0;
    constexpr std::uint64_t full_period_frames = 4114286U;
    constexpr std::uint64_t tail_frames = rate_hz / 10U;
    const std::uint64_t frame_count = full_period_frames + tail_frames;
    constexpr std::array<std::uint64_t, 3U> golden_frames{2057141U, 2057142U, 2057143U};
    // Independent continuous-phase reference at the actual 48 kHz frame
    // positions around the +75 Hz turn; the noninteger half-period is retained.
    constexpr std::array<double, 3U> golden_output{-0.3599404119511568, -0.3077079232678435, -0.2493924554072397};
    // Exact IEEE-754 binary32 results under the project's strict, no-contraction
    // float contract; the same bit checks are exercised on Linux CI.
    constexpr std::array<std::uint32_t, 3U> golden_output_bits{0xbeb84a2cU, 0xbe9d8becU, 0xbe7f60b9U};
    std::array<float, 64U> block{};
    std::array<float, golden_frames.size()> golden_actual{};
    std::vector<double> positive_crossings;
    positive_crossings.reserve(static_cast<std::size_t>(frame_count / 100U));
    std::uint64_t first_frame = 0U;
    float previous_output = 0.0F;
    bool have_previous = false;

    while (first_frame < frame_count)
    {
        const std::size_t count = static_cast<std::size_t>(std::min<std::uint64_t>(block.size(), frame_count - first_frame));

        for (std::size_t index = 0U; index < count; ++index)
        {
            const std::uint64_t frame = first_frame + index;
            block[index] = static_cast<float>(0.5 * sl::det::cos(sl::det::two_pi * source_hz * static_cast<double>(frame) / rate_hz));
        }

        const auto processed = stage.process(block.data(), count, first_frame, block.size());

        if (!processed.has_value() || processed.value() != count)
        {
            FAIL("frequency shift rejected a contiguous full-period test block");
        }

        for (std::size_t index = 0U; index < count; ++index)
        {
            const std::uint64_t frame = first_frame + index;
            const float output = block[index];

            for (std::size_t golden = 0U; golden < golden_frames.size(); ++golden)
            {
                if (frame == golden_frames[golden])
                {
                    golden_actual[golden] = output;
                }
            }

            if (have_previous && previous_output <= 0.0F && output > 0.0F)
            {
                const double fraction = -static_cast<double>(previous_output) / (static_cast<double>(output) - previous_output);
                positive_crossings.push_back(static_cast<double>(frame - 1U) + fraction);
            }

            previous_output = output;
            have_previous = true;
        }

        first_frame += count;
    }

    for (std::size_t index = 0U; index < golden_actual.size(); ++index)
    {
        CHECK(std::abs(static_cast<double>(golden_actual[index]) - golden_output[index]) < 2.0e-3);
        CHECK(std::bit_cast<std::uint32_t>(golden_actual[index]) == golden_output_bits[index]);
    }

    const double half_period = (150.0 / 3.5) * static_cast<double>(rate_hz);
    const double quarter_leg = half_period / 4.0;
    const double first_leg_frequency = measured_frequency(positive_crossings, quarter_leg) - source_hz;
    const double third_leg_frequency = measured_frequency(positive_crossings, 3.0 * quarter_leg) - source_hz;
    const double fitted_slope = (third_leg_frequency - first_leg_frequency) * static_cast<double>(rate_hz) / (2.0 * quarter_leg);
    const double measured_upper_turn = measured_frequency(positive_crossings, half_period) - source_hz;
    const double measured_lower_turn = measured_frequency(positive_crossings, 2.0 * half_period) - source_hz;
    const double measured_period_seconds = 4.0 * 75.0 / fitted_slope;
    CHECK(std::abs(measured_upper_turn - 75.0) <= 0.1);
    CHECK(std::abs(measured_lower_turn + 75.0) <= 0.1);
    CHECK(std::abs(first_leg_frequency + 37.5) < 0.05);
    CHECK(std::abs(third_leg_frequency - 37.5) < 0.05);
    CHECK(std::abs(fitted_slope - 3.5) < 0.02);
    CHECK(std::abs(measured_period_seconds - (300.0 / 3.5)) < 0.5);
}

TEST_CASE("frequency shift: phase increment has a stable 64-bit known answer", "[frequency_shift]")
{
    for (const auto& [offset_hz, expected_phase] : std::array<std::pair<double, std::uint64_t>, 2U>{{{75.0, 0x0066666666666668ULL}, {-75.0, 0xff99999999999998ULL}}})
    {
        sl::FrequencyShiftParams params{};
        params.offset = units::FrequencyDelta{offset_hz, units::Hz};
        sl::FrequencyShiftStage stage{params};
        REQUIRE(stage.prepare(sl::StageContext{}).is_ok());
        std::array<float, 1U> sample{0.25F};
        REQUIRE(stage.process(sample.data(), sample.size(), 0U, sample.size()).has_value());
        CHECK(stage.phase_word() == expected_phase);
    }
}

TEST_CASE("frequency shift: signed offsets match an independent settled waveform vector", "[frequency_shift]")
{
    // Literal reference values for a 0.5-amplitude, 1 kHz cosine, delayed by
    // the Hilbert center (215 frames) and shifted by +/-75 Hz. The samples are
    // deliberately taken after the FIR has settled, and do not reuse its phase
    // or carrier-increment calculations.
    constexpr std::array<std::size_t, 3U> sample_frames{512U, 513U, 514U};
    constexpr std::array<std::array<double, 3U>, 2U> expected{{
        {{0.49845866686656404, 0.49903380548139814, 0.48974370977952620}},
        {{-0.38020298280001610, -0.41664162810387695, -0.44697938998496540}},
    }};
    constexpr std::array<std::array<std::uint32_t, 3U>, 2U> expected_bits{{
        {{0x3eff3a0eU, 0x3eff858fU, 0x3efac401U}},
        {{0xbec2ae0cU, 0xbed55641U, 0xbee4decdU}},
    }};
    constexpr std::array<double, 2U> offsets{75.0, -75.0};

    for (std::size_t sign = 0U; sign < offsets.size(); ++sign)
    {
        sl::FrequencyShiftParams params{};
        params.offset = units::FrequencyDelta{offsets[sign], units::Hz};
        sl::FrequencyShiftStage stage{params};
        REQUIRE(stage.prepare(sl::StageContext{}).is_ok());
        std::vector<float> samples(1024U);

        for (std::size_t index = 0U; index < samples.size(); ++index)
        {
            samples[index] = static_cast<float>(0.5 * sl::det::cos(sl::det::two_pi * 1000.0 * static_cast<double>(index) / rate_hz));
        }

        REQUIRE(stage.process(samples.data(), samples.size(), 0U, samples.size()).has_value());

        for (std::size_t check = 0U; check < sample_frames.size(); ++check)
        {
            CHECK(std::abs(static_cast<double>(samples[sample_frames[check]]) - expected[sign][check]) < 2.0e-3);
            CHECK(std::bit_cast<std::uint32_t>(samples[sample_frames[check]]) == expected_bits[sign][check]);
        }
    }
}

TEST_CASE("frequency shift: the positive offset moves a tone by the requested amount", "[frequency_shift]")
{
    sl::FrequencyShiftParams params{};
    params.offset = units::FrequencyDelta{75.0, units::Hz};
    sl::FrequencyShiftStage stage{params};
    CHECK(stage.prepare(sl::StageContext{}).is_ok());
    constexpr std::size_t frame_count = std::size_t{2U} * rate_hz;
    std::vector<float> samples(frame_count);

    for (std::size_t index = 0U; index < frame_count; ++index)
    {
        samples[index] = static_cast<float>(0.5 * sl::det::cos(sl::det::two_pi * 1000.0 * static_cast<double>(index) / rate_hz));
    }

    CHECK(stage.process(samples.data(), samples.size(), 0U, samples.size()).value() == frame_count);
    const std::span<const float> settled{samples.data() + rate_hz, rate_hz};
    const auto shifted = signal_lab_test::correlate(settled, 1075.0, rate_hz);
    const auto unshifted = signal_lab_test::correlate(settled, 1000.0, rate_hz);
    const auto wrong_side = signal_lab_test::correlate(settled, 925.0, rate_hz);
    CHECK(std::abs(std::abs(shifted) - 0.25) < 1.0e-3);
    CHECK(std::abs(unshifted) < 1.0e-4);
    CHECK(std::abs(wrong_side) < 1.0e-4);
}

TEST_CASE("frequency shift: +/-75 Hz leaves no measurable positive-band spur above -60 dBc", "[frequency_shift]")
{
    constexpr std::size_t fft_size = 65536U;
    constexpr std::size_t warmup = 4096U;
    constexpr std::size_t main_bin = 2048U; // 1500 Hz at 48 kHz / 65536.
    constexpr double spur_limit_db = -60.0;

    for (const auto& [source_hz, offset_hz] : std::array<std::pair<double, double>, 2U>{{{1425.0, 75.0}, {1575.0, -75.0}}})
    {
        sl::FrequencyShiftParams params{};
        params.offset = units::FrequencyDelta{offset_hz, units::Hz};
        sl::FrequencyShiftStage stage{params};
        REQUIRE(stage.prepare(sl::StageContext{}).is_ok());
        std::vector<float> samples(warmup + fft_size);

        for (std::size_t index = 0U; index < samples.size(); ++index)
        {
            samples[index] = static_cast<float>(0.5 * sl::det::cos(sl::det::two_pi * source_hz * static_cast<double>(index) / rate_hz));
        }

        REQUIRE(stage.process(samples.data(), samples.size(), 0U, samples.size()).value() == samples.size());
        std::vector<signal_lab_test::Complex> analytic_real(fft_size);

        for (std::size_t index = 0U; index < fft_size; ++index)
        {
            analytic_real[index] = static_cast<double>(samples[warmup + index]);
        }

        const std::vector<double> power = signal_lab_test::average_periodogram(analytic_real, fft_size);
        double largest_spur = 0.0;

        for (std::size_t bin = 0U; bin < fft_size / 2U; ++bin)
        {
            if (bin != main_bin)
            {
                largest_spur = std::max(largest_spur, power[bin]);
            }
        }

        CHECK(power[main_bin] > 0.0);
        CHECK(signal_lab_test::power_db(largest_spur / power[main_bin]) < spur_limit_db);
    }
}

TEST_CASE("frequency shift: phase and filter output do not depend on block partition", "[frequency_shift]")
{
    sl::FrequencyShiftParams params{};
    params.mode = sl::FrequencyShiftMode::sweep;
    params.amplitude = units::FrequencyDelta{40.0, units::Hz};
    params.start_frequency = units::FrequencyDelta{-40.0, units::Hz};
    // This accelerated sweep has a fractional-frame period and crosses turns
    // inside the block, while keeping its Q0.64 cycle step below 2^53.
    params.slope = sl::FrequencySlope{2500.0};
    constexpr std::size_t frame_count = 4096U;
    std::vector<float> input(frame_count);

    for (std::size_t index = 0U; index < frame_count; ++index)
    {
        input[index] = static_cast<float>(0.4 * sl::det::sin(sl::det::two_pi * 1000.0 * static_cast<double>(index) / rate_hz));
    }

    std::vector<float> whole = input;
    std::vector<float> pieces = input;
    sl::FrequencyShiftStage one{params};
    sl::FrequencyShiftStage many{params};
    CHECK(one.prepare(sl::StageContext{}).is_ok());
    CHECK(many.prepare(sl::StageContext{}).is_ok());
    CHECK(one.process(whole.data(), whole.size(), 0U, whole.size()).value() == frame_count);
    constexpr std::array<std::size_t, 8U> partition{1U, 63U, 64U, 127U, 1024U, 17U, 2048U, 511U};
    std::size_t cursor = 0U;
    std::size_t part = 0U;

    while (cursor < frame_count)
    {
        const std::size_t count = std::min(partition[part % partition.size()], frame_count - cursor);
        CHECK(many.process(pieces.data() + cursor, count, cursor, count).value() == count);
        cursor += count;
        ++part;
    }

    CHECK(pieces == whole);
    CHECK(many.phase_word() == one.phase_word());
}

TEST_CASE("frequency shift: gain processing matches an independent analytic-signal calculation", "[frequency_shift]")
{
    constexpr std::size_t frame_count = 257U;
    std::array<float, frame_count> input{};

    for (std::size_t index = 0U; index < input.size(); ++index)
    {
        input[index] = static_cast<float>(0.4 * sl::det::sin(sl::det::two_pi * 1300.0 * static_cast<double>(index) / rate_hz));
    }

    std::array<sl::AnalyticSample, frame_count> analytic{};
    sl::AnalyticFilter oracle_filter{};
    oracle_filter.configure();

    for (std::size_t offset = 0U; offset < input.size(); offset += sl::analytic_chunk_frames.value())
    {
        const std::size_t count = std::min<std::size_t>(sl::analytic_chunk_frames.value(), input.size() - offset);
        oracle_filter.process(std::span<const float>{input}.subspan(offset, count), units::SampleIndex{offset}, std::span<sl::AnalyticSample>{analytic}.subspan(offset, count));
    }

    sl::FrequencyShiftParams params{};
    sl::FrequencyShiftStage stage{params};
    REQUIRE(stage.prepare(sl::StageContext{}).is_ok());
    std::array<sl::ComplexFloat, frame_count> gains{};
    gains.fill(sl::ComplexFloat{0.0F, 1.0F});
    auto actual = input;
    REQUIRE(stage.process_with_gain(actual.data(), actual.size(), 0U, actual.size(), gains).value() == frame_count);

    for (std::size_t index = 0U; index < actual.size(); ++index)
    {
        // With a zero carrier phase, multiplication by +j maps I+jQ to -Q+jI.
        CHECK(actual[index] == -analytic[index].quadrature);
    }
}

TEST_CASE("frequency shift: invalid gain and block geometry leave filter and NCO retryable", "[frequency_shift]")
{
    sl::FrequencyShiftParams params{};
    params.offset = units::FrequencyDelta{37.0, units::Hz};
    sl::FrequencyShiftStage retryable{params};
    sl::FrequencyShiftStage reference{params};
    REQUIRE(retryable.prepare(sl::StageContext{}).is_ok());
    REQUIRE(reference.prepare(sl::StageContext{}).is_ok());
    std::array<float, 128U> candidate{};
    std::array<float, 128U> expected{};

    for (std::size_t index = 0U; index < candidate.size(); ++index)
    {
        candidate[index] = expected[index] = static_cast<float>(0.3 * sl::det::cos(sl::det::two_pi * 900.0 * static_cast<double>(index) / rate_hz));
    }

    const auto original = candidate;
    std::array<sl::ComplexFloat, 128U> gains{};
    gains.fill(sl::ComplexFloat{1.0F, 0.0F});
    CHECK_FALSE(retryable.process_with_gain(candidate.data(), candidate.size(), 0U, candidate.size(), std::span<const sl::ComplexFloat>{gains}.first(127U)).has_value());
    CHECK(candidate == original);
    gains[37U].imag = std::numeric_limits<float>::infinity();
    CHECK_FALSE(retryable.process_with_gain(candidate.data(), candidate.size(), 0U, candidate.size(), gains).has_value());
    CHECK(candidate == original);
    gains[37U].imag = 0.0F;
    CHECK_FALSE(retryable.process_with_gain(candidate.data(), candidate.size(), 0U, candidate.size() - 1U, gains).has_value());
    CHECK(candidate == original);
    REQUIRE(retryable.process_with_gain(candidate.data(), candidate.size(), 0U, candidate.size(), gains).has_value());
    REQUIRE(reference.process(expected.data(), expected.size(), 0U, expected.size()).has_value());
    CHECK(candidate == expected);
    CHECK(retryable.phase_word() == reference.phase_word());
}

} // namespace
