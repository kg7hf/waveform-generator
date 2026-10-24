// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// Catch2 suite of the Watterson channel (signal_lab/watterson.hpp): design 3 to 8,
// tests U7 to U9, U13 (b, c), U15, U18 (b, c), U19 to U24, and the static-storage
// contract. Guide: docs/modules/signal-lab-watterson.md.
//
// The channel's output is a function of (seed, stage, path, absolute frame) only,
// so most cases here are exact: bit-for-bit against a reference built in this file
// from the stage's own nodes (TapObserver) and coefficients, with the stage's
// documented float order but none of its chunking. The statistical cases use fixed
// seeds; each tolerance is at least 5 standard errors (design 10.1).

#include "signal_lab/analytic_filter.hpp"
#include "signal_lab/convert.hpp"
#include "signal_lab/det_math.hpp"
#include "signal_lab/units/conversions.hpp"
#include "signal_lab/watterson.hpp"
#include "test_convert.hpp"
#include "test_spectrum.hpp"
#include "test_taps.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <numbers>
#include <span>
#include <type_traits>
#include <vector>

namespace
{

namespace sl = signal_lab;
namespace units = signal_lab::units;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using signal_lab_test::Complex;
using signal_lab_test::two_pi;

constexpr double rate_hz = 48000.0;
constexpr double frames_per_ms = rate_hz / 1000.0;
constexpr std::uint64_t seed_one = 1U;
constexpr std::uint64_t seed_two = 2U;
constexpr std::uint32_t stage_zero = 0U;
constexpr double one_hertz = 1.0;
constexpr double ten_hertz = 10.0;
// The engine's block: 2048 frames (impairment.hpp's engine_block_frames).
constexpr std::size_t engine_block = 2048U;

// ---- the static-storage contract: no allocation, compile-time size ----
static_assert(std::is_trivially_copyable_v<sl::WattersonChannel>, "the channel is plain bytes: static, NOLOAD-placeable");
static_assert(sizeof(sl::WattersonChannel) <= sl::watterson_channel_budget_bytes, "within the 20 KiB budget (design 11.2)");
static_assert(sl::analytic_ring_frames.value() >= sl::watterson_max_delay_frames.value() + sl::analytic_chunk_frames.value(), "design 4.2");
// The 48 kHz capacities of design 3.3, 4.1, 4.2 and 5.3.
constexpr std::uint32_t design_paths = 4U;
constexpr std::uint32_t design_hilbert_taps = 431U;
constexpr std::uint32_t design_hilbert_pairs = 108U;
constexpr std::uint64_t design_ring_frames = 1024U;
constexpr std::uint64_t design_max_delay_frames = 960U;
constexpr std::uint32_t design_doppler_taps = 91U; // 111 if Q1 selects 0.1 %
static_assert(sl::watterson_max_paths == design_paths && sl::hilbert_taps == design_hilbert_taps && sl::hilbert_pairs == design_hilbert_pairs, "compile-time capacities");
static_assert(sl::analytic_ring_frames.value() == design_ring_frames && sl::watterson_max_delay_frames.value() == design_max_delay_frames && sl::doppler_max_taps == design_doppler_taps,
              "compile-time capacities");
static_assert(std::is_nothrow_default_constructible_v<sl::WattersonChannel>, "construction cannot fail");

// The board's declaration form: constant-initialised static storage.
[[nodiscard]] sl::WattersonChannel& static_channel()
{
    constinit static sl::WattersonChannel channel{};
    return channel;
}

[[nodiscard]] sl::WattersonPath fixed_path(double delay_ms, double level_db = 0.0, double phase_degrees = 0.0, double shift_hz = 0.0)
{
    sl::WattersonPath path{};
    path.fading = sl::PathFading::fixed;
    path.delay = units::Duration{delay_ms, units::ms};
    path.level = units::Gain{level_db, units::dB};
    path.phase = units::Angle{phase_degrees, units::degrees};
    path.shift = units::FrequencyDelta{shift_hz, units::Hz};
    return path;
}

[[nodiscard]] sl::WattersonPath rayleigh_path(double delay_ms, double spread_hz, double level_db = 0.0, double shift_hz = 0.0)
{
    sl::WattersonPath path{};
    path.fading = sl::PathFading::rayleigh;
    path.delay = units::Duration{delay_ms, units::ms};
    path.level = units::Gain{level_db, units::dB};
    path.spread = units::FrequencyDelta{spread_hz, units::Hz};
    path.shift = units::FrequencyDelta{shift_hz, units::Hz};
    return path;
}

[[nodiscard]] sl::WattersonParams params_of(std::span<const sl::WattersonPath> paths)
{
    sl::WattersonParams params{};
    std::ranges::copy(paths, params.paths.begin());
    params.path_count = sl::narrow_count(paths.size());
    return params;
}

[[nodiscard]] double delay_in_ms(std::uint64_t frames)
{
    return sl::as_double(frames) / frames_per_ms;
}

// A seeded noise input, uniform in +/-0.5: broadband, so every frame matters.
constexpr double noise_centre = 0.5;
constexpr std::uint64_t input_seed = 99U;
constexpr std::uint32_t input_family = 3U;

[[nodiscard]] std::vector<float> noise_input(std::size_t frames)
{
    sl::det::Pcg32 generator{input_seed, input_family, 0U};
    std::vector<float> input(frames);

    for (float& sample : input)
    {
        sample = sl::to_float(generator.uniform() - noise_centre);
    }

    return input;
}

[[nodiscard]] std::vector<float> tone_input(std::size_t frames, double frequency)
{
    std::vector<float> input(frames);

    for (std::size_t frame = 0U; frame < frames; ++frame)
    {
        input[frame] = sl::to_float(std::cos(two_pi * frequency * sl::as_double(frame) / rate_hz));
    }

    return input;
}

[[nodiscard]] std::uint64_t digest_of(std::span<const float> samples)
{
    sl::det::StreamDigest digest;
    digest.update(samples.data(), samples.size());
    return digest.value();
}

// ---- the reference model: frame by frame, no chunks, from the stage's nodes ----

// xa(n) by the definition of design 3.1, from the whole input history.
[[nodiscard]] std::vector<sl::AnalyticSample> reference_analytic(std::span<const float> input, std::span<const float, sl::hilbert_pairs> coefficients)
{
    const auto sample = [&input](std::int64_t frame) { return frame < 0 ? 0.0F : input[signal_lab_test::index_of(frame)]; };

    std::vector<sl::AnalyticSample> analytic(input.size());
    const std::int64_t delay{sl::hilbert_delay};

    for (std::size_t frame = 0U; frame < input.size(); ++frame)
    {
        const std::int64_t centre = signal_lab_test::signed_frame(frame) - delay;
        float quadrature = 0.0F;

        for (std::size_t pair = 0U; pair < sl::hilbert_pairs; ++pair)
        {
            const std::int64_t offset = signal_lab_test::signed_frame(2U * pair + 1U);
            quadrature += coefficients[pair] * (sample(centre - offset) - sample(centre + offset));
        }

        analytic[frame] = sl::AnalyticSample{sample(centre), quadrature};
    }

    return analytic;
}

[[nodiscard]] sl::ComplexDouble reference_rotation(double angle)
{
    return sl::ComplexDouble{sl::det::cos(angle), sl::det::sin(angle)};
}

// The 48 kHz gain g_i(n) of one path, n = 0 ... frames - 1, by design 4.5 and 5.4.
[[nodiscard]] std::vector<sl::ComplexFloat> reference_gain(const sl::WattersonPathReport& report, std::span<const sl::TapRecord> records, std::size_t frames)
{
    std::vector<sl::ComplexFloat> gains(frames);
    const std::uint32_t increment = report.phase_step.word();
    const bool rotates = increment != 0U;
    const bool rayleigh = report.fading == sl::PathFading::rayleigh;
    const double phase = rayleigh ? 0.0 : report.phase.in(units::radians);
    const sl::ComplexFloat base = rotates ? sl::ComplexFloat{sl::to_float(report.amplitude), 0.0F} : records.front().value;
    const double step_angle = sl::det::two_pi * sl::as_double(sl::as_signed(increment)) / units::phase_turn;
    const sl::ComplexDouble step = reference_rotation(step_angle);
    const std::uint64_t node_frames = report.frames_per_node.value();
    const double inverse = rayleigh ? 1.0 / sl::as_double(node_frames) : 0.0;
    sl::ComplexDouble rotator{};

    for (std::size_t frame = 0U; frame < frames; ++frame)
    {
        sl::ComplexFloat gain = base;

        if (rayleigh)
        {
            const std::size_t node = frame / node_frames;
            const sl::ComplexFloat now = records[node].value;
            const sl::ComplexFloat next = records[node + 1U].value;
            const auto mu = sl::to_float(sl::as_double(frame % node_frames) * inverse);
            gain = sl::ComplexFloat{now.real + mu * (next.real - now.real), now.imag + mu * (next.imag - now.imag)};
        }

        if (rotates)
        {
            if (frame % sl::rotator_resync_frames == 0U)
            {
                const std::uint32_t word = sl::low_word(std::uint64_t{frame} * increment);
                rotator = reference_rotation(sl::det::two_pi * sl::as_double(word) / units::phase_turn + phase);
            }

            const sl::ComplexFloat turn{sl::to_float(rotator.real), sl::to_float(rotator.imag)};
            gain = sl::ComplexFloat{(gain.real * turn.real) - (gain.imag * turn.imag), (gain.real * turn.imag) + (gain.imag * turn.real)};
            rotator = sl::ComplexDouble{(rotator.real * step.real) - (rotator.imag * step.imag), (rotator.real * step.imag) + (rotator.imag * step.real)};
        }

        gains[frame] = gain;
    }

    return gains;
}

[[nodiscard]] std::vector<float> reference_output(const sl::WattersonChannel& channel, const signal_lab_test::TapRecorder& recorder, std::span<const float> input)
{
    const std::vector<sl::AnalyticSample> analytic = reference_analytic(input, channel.hilbert_coefficients());
    const sl::WattersonReport& report = channel.report();
    std::vector<float> output(input.size(), 0.0F);

    for (std::uint32_t path = 0U; path < report.path_count; ++path)
    {
        const sl::WattersonPathReport& realised = report.paths.at(path);
        const std::vector<sl::TapRecord> records = recorder.path(path);
        const std::vector<sl::ComplexFloat> gains = reference_gain(realised, records, input.size());
        const std::uint64_t delay = realised.delay.value();

        for (std::size_t frame = 0U; frame < input.size(); ++frame)
        {
            const sl::AnalyticSample x = frame >= delay ? analytic[frame - delay] : sl::AnalyticSample{};
            const float term = (gains[frame].real * x.in_phase) - (gains[frame].imag * x.quadrature);
            output[frame] = path == 0U ? term : output[frame] + term;
        }
    }

    return output;
}

// The first frame where two runs differ, or the size when they agree.
[[nodiscard]] std::size_t first_difference(std::span<const float> first, std::span<const float> second)
{
    const auto mismatch = std::ranges::mismatch(first, second, [](float a, float b) { return std::bit_cast<std::uint32_t>(a) == std::bit_cast<std::uint32_t>(b); });

    return signal_lab_test::count_of(mismatch.in1 - first.begin());
}

// ---- case data ----

// U7/U8: the delays of the impulse test, frames (design 10.1).
constexpr std::array impulse_delays{0U, 1U, 24U, 96U, 144U, 240U, 432U, 960U};
constexpr std::array impulse_blocks{std::size_t{1}, std::size_t{96}, std::size_t{128}, std::size_t{448}, std::size_t{2048}};
constexpr std::size_t impulse_frame = 1000U;
constexpr std::size_t impulse_frames = 2400U;
// "Nothing else above -120 dB": the identity path makes every other sample exactly 0.
constexpr double impulse_floor = 1.0e-6;
// U8 with four fixed paths: levels in dB at four delays.
constexpr std::array four_levels_db{0.0, -3.0, -6.0, -10.0};
constexpr std::array four_delays{0U, 24U, 432U, 960U};
// a_i = sqrt(p_i) is stored as a float: half an ulp of relative error in a_i^2.
constexpr double float_power_bound = 2.5e-7;

// U9: phases and shifts of a fixed path.
constexpr std::array phase_cases_degrees{30.0, -90.0, 135.0, 360.0};
constexpr double tone_hz = 1700.0;
constexpr std::array shift_cases_hz{75.0, -75.0};
constexpr std::size_t shift_seconds = 10U;
constexpr std::size_t shift_block_frames = 4800U; // 0.1 s per phase estimate
constexpr double frequency_bound_hz = 1.0e-4;
constexpr double mirror_limit_db = -70.0;
constexpr std::size_t settle_frames = 2048U;

// U13 (b): spreads with M = 7500, 150 and 75.
constexpr std::array interpolation_spreads_hz{0.1, 5.0, 10.0};
constexpr std::size_t interpolation_frames = 60000U;
// U13 (c): the image power at k R, k = 1 to 3, of the 48 kHz gain at d = 5 Hz.
constexpr double image_spread_hz = 5.0;
constexpr std::size_t image_segment = std::size_t{1} << 18U; // 0.18 Hz bins, below sigma / 10
constexpr std::size_t image_segments = 8U;
constexpr double image_limit_db = -75.0;
constexpr double image_window_sigmas = 4.0;
constexpr std::array image_orders{1.0, 2.0, 3.0, -1.0, -2.0, -3.0};

// U15: four Rayleigh paths at the set levels; normalisation exact to 1e-12.
constexpr double normalisation_bound = 1.0e-12;
constexpr std::array four_spreads_hz{1.0, 0.5, 2.0, 10.0};

// U18 (b): the realised 48 kHz interval mean equals the interpolation-weight form to 1e-6.
constexpr double interval_bound = 1.0e-6;
// U18 (c): p_i = 1, 0.5 and 0.1 (levels 0 and 10 log10 9 = 9.542 dB make 0.1 and 0.9).
constexpr double ninth_db = 9.5424250943932487;
constexpr double tenth = 0.1;
constexpr double half_power = 0.5;

// U19: fixed 0 dB plus Rayleigh -10 dB at 0 ms: K = 10 dB +/- 0.5 dB from the node moments.
// 2^18 nodes at 10 Hz; a Monte Carlo of this estimator gives a standard deviation of
// 0.081 dB at that length, so 0.5 dB is 6 of them.
constexpr double rician_k_db = 10.0;
constexpr double rician_bound_db = 0.5;
constexpr std::size_t rician_nodes = std::size_t{1} << 18U;
constexpr double moment_two = 2.0;

// U21: block partitions (design 10.1).
constexpr std::array partition_blocks{std::size_t{1},   std::size_t{7},   std::size_t{63},  std::size_t{64},  std::size_t{65},   std::size_t{96},
                                      std::size_t{255}, std::size_t{256}, std::size_t{257}, std::size_t{448}, std::size_t{1000}, std::size_t{2048}};
constexpr std::size_t partition_frames = 40000U;
constexpr std::uint64_t partition_seed = 0x5EEDU;
constexpr std::size_t random_partition_blocks = 64U;
constexpr std::uint32_t random_partition_limit = 2048U;
constexpr double fixed_phase_degrees = 30.0;
constexpr double small_shift_hz = -0.01;
constexpr double right_angle_degrees = -90.0;
constexpr double weaker_db = -6.0;
constexpr double full_shift_hz = 75.0;
constexpr double longest_delay_ms = 20.0;
constexpr double fixed_delay_ms = 4.5;
constexpr double short_delay_ms = 1.0;
constexpr double slowest_hz = 0.1;

// U23: the golden values of the reference scenario at seed 1. They pin the draw order,
// the float order and the normalisation; change them only with a documented reason.
// Recorded 2026-10-09 with algorithm version 1 and 1 % truncation (Q1 open), from the
// first implementation; the output digest is of the PCM24-quantised output.
constexpr std::uint64_t golden_parameter_digest = 0x0DECE67AA2321745ULL;
constexpr std::uint64_t golden_output_digest = 0x79B8F15994393F2FULL;
constexpr float golden_first_node_real = -0x1.6f9be2p-3F;
constexpr float golden_first_node_imag = 0x1.1765d4p-2F;

// The rate in whole frames per second, and the base of the decibel.
constexpr std::size_t rate_frames = 48000U;
constexpr double decade = 10.0;
// U15 and U22: the second path of a pair, and the changes U22 makes to it.
constexpr double second_delay_ms = 2.0;
constexpr double half_hertz = 0.5;
constexpr double other_delay_ms = 9.0;
constexpr double other_spread_hz = 7.0;
constexpr double other_shift_hz = 30.0;
constexpr double other_phase_degrees = 45.0;
// U24: the byte that fills a NOLOAD model of the channel before configure().
constexpr std::byte poison_byte{0xFFU};

// The samples other than `peak` above the -120 dB floor.
[[nodiscard]] std::size_t stray_samples(std::span<const float> output, std::size_t peak)
{
    std::size_t stray = 0U;

    for (std::size_t frame = 0U; frame < output.size(); ++frame)
    {
        stray += frame != peak && std::abs(output[frame]) > impulse_floor ? 1U : 0U;
    }

    return stray;
}

// a_i^2 v_i: the power a path delivers at 48 kHz.
[[nodiscard]] double realised_power(const sl::WattersonPathReport& path)
{
    return path.amplitude * path.amplitude * path.interpolation_power;
}

// The reference scenario of U20, U21, U23 and U24: every kind of path.
[[nodiscard]] sl::WattersonParams reference_params()
{
    const std::array paths{rayleigh_path(0.0, slowest_hz), rayleigh_path(longest_delay_ms, ten_hertz, 0.0, full_shift_hz), fixed_path(fixed_delay_ms, 0.0, fixed_phase_degrees, small_shift_hz),
                           fixed_path(short_delay_ms, weaker_db, right_angle_degrees)};
    return params_of(paths);
}

} // namespace

TEST_CASE("watterson: one fixed path reproduces the input delayed by D frames, bit for bit (U7)", "[watterson]")
{
    auto channel = signal_lab_test::make_channel();
    const std::array paths{fixed_path(0.0)};
    REQUIRE(channel->configure(params_of(paths), seed_one, stage_zero, nullptr).is_ok());
    const std::vector<float> input = noise_input(partition_frames);
    bool ok = false;
    const std::vector<float> output = signal_lab_test::run(*channel, input, engine_block, ok);
    REQUIRE(ok);
    std::vector<float> expected(input.size(), 0.0F);
    std::copy_n(input.begin(), input.size() - sl::hilbert_delay, std::next(expected.begin(), sl::hilbert_delay));
    CHECK(first_difference(output, expected) == output.size());
    CHECK(channel->report().latency == sl::hilbert_latency);
    CHECK(channel->report().paths[0].amplitude == 1.0);
}

TEST_CASE("watterson: an impulse at frame 1000 comes out at 1000 + D + L at every block size (U8)", "[watterson]")
{
    auto channel = signal_lab_test::make_channel();
    std::vector<float> input(impulse_frames, 0.0F);
    input[impulse_frame] = 1.0F;

    for (const std::uint32_t delay : impulse_delays)
    {
        for (const std::size_t block : impulse_blocks)
        {
            const std::array paths{fixed_path(delay_in_ms(delay))};
            REQUIRE(channel->configure(params_of(paths), seed_one, stage_zero, nullptr).is_ok());
            REQUIRE(channel->report().paths[0].delay.value() == delay);
            bool ok = false;
            const std::vector<float> output = signal_lab_test::run(*channel, input, block, ok);
            REQUIRE(ok);
            const std::size_t peak = impulse_frame + sl::hilbert_delay + delay;
            INFO("L = " << delay << ", block " << block);
            CHECK(output[peak] == 1.0F);
            CHECK(stray_samples(output, peak) == 0U);
        }
    }
}

TEST_CASE("watterson: four fixed paths give their normalised amplitudes at their delays (U8, U15)", "[watterson]")
{
    auto channel = signal_lab_test::make_channel();
    std::array<sl::WattersonPath, four_levels_db.size()> paths{};
    double total = 0.0;

    for (std::size_t index = 0U; index < paths.size(); ++index)
    {
        paths.at(index) = fixed_path(delay_in_ms(four_delays.at(index)), four_levels_db.at(index));
        total += std::pow(decade, four_levels_db.at(index) / signal_lab_test::power_db_per_decade);
    }

    REQUIRE(channel->configure(params_of(paths), seed_one, stage_zero, nullptr).is_ok());
    std::vector<float> input(impulse_frames, 0.0F);
    input[impulse_frame] = 1.0F;
    bool ok = false;
    const std::vector<float> output = signal_lab_test::run(*channel, input, engine_block, ok);
    REQUIRE(ok);
    double sum = 0.0;

    for (std::size_t index = 0U; index < paths.size(); ++index)
    {
        const double expected = std::pow(decade, four_levels_db.at(index) / signal_lab_test::power_db_per_decade) / total;
        const auto peak = sl::as_double(output[impulse_frame + sl::hilbert_delay + four_delays.at(index)]);
        INFO("path " << index);
        CHECK_THAT(peak * peak, WithinRel(expected, float_power_bound));
        sum += peak * peak;
    }

    CHECK_THAT(sum, WithinAbs(1.0, float_power_bound));
}

TEST_CASE("watterson: a fixed path's phase gives cos(phi) xI - sin(phi) xQ, bit for bit (U9)", "[watterson]")
{
    auto channel = signal_lab_test::make_channel();
    const std::vector<float> input = noise_input(impulse_frames);

    for (const double phase : phase_cases_degrees)
    {
        signal_lab_test::TapRecorder recorder;
        const std::array paths{fixed_path(0.0, 0.0, phase)};
        REQUIRE(channel->configure(params_of(paths), seed_one, stage_zero, &recorder).is_ok());
        bool ok = false;
        const std::vector<float> output = signal_lab_test::run(*channel, input, engine_block, ok);
        REQUIRE(ok);
        const std::vector<sl::TapRecord> records = recorder.path(0U);
        REQUIRE(records.size() == 1U);
        const double radians = phase * std::numbers::pi / units::degrees_per_half_turn;
        INFO("phase " << phase << " degrees");
        CHECK_THAT(sl::as_double(records[0].value.real), WithinAbs(std::cos(radians), float_power_bound));
        CHECK_THAT(sl::as_double(records[0].value.imag), WithinAbs(std::sin(radians), float_power_bound));
        CHECK(first_difference(output, reference_output(*channel, recorder, input)) == output.size());
    }
}

TEST_CASE("watterson: a shift moves a tone by f to 1e-4 Hz, with the mirror at most -70 dBc (U9)", "[watterson]")
{
    auto channel = signal_lab_test::make_channel();
    const std::size_t frames = shift_seconds * rate_frames + settle_frames;
    const std::vector<float> input = tone_input(frames, tone_hz);

    for (const double shift : shift_cases_hz)
    {
        const std::array paths{fixed_path(0.0, 0.0, 0.0, shift)};
        REQUIRE(channel->configure(params_of(paths), seed_one, stage_zero, nullptr).is_ok());
        bool ok = false;
        const std::vector<float> output = signal_lab_test::run(*channel, input, engine_block, ok);
        REQUIRE(ok);
        const double realised = channel->report().paths[0].realised_shift.in(units::Hz);
        CHECK(std::abs(realised - shift) <= rate_hz / units::phase_turn);
        const double expected = tone_hz + realised;
        std::vector<double> times;
        std::vector<double> phases;

        for (std::size_t start = settle_frames; start + shift_block_frames <= frames; start += shift_block_frames)
        {
            const std::span<const float> part = std::span{output}.subspan(start, shift_block_frames);
            // The correlation's phase at absolute time: rotate back by the block's start.
            const double start_angle = two_pi * expected * sl::as_double(start) / rate_hz;
            const Complex sum = signal_lab_test::correlate(part, expected, rate_hz) * Complex{std::cos(-start_angle), std::sin(-start_angle)};
            const double phase = std::arg(sum);
            const double previous = phases.empty() ? phase : phases.back();
            phases.push_back(previous + std::remainder(phase - previous, two_pi));
            times.push_back(sl::as_double(start) / rate_hz);
        }

        const signal_lab_test::Line line = signal_lab_test::fit_line(times, phases);
        const std::span<const float> second = std::span{output}.subspan(settle_frames, rate_frames);
        const double wanted = std::abs(signal_lab_test::correlate(second, expected, rate_hz));
        const double mirror = std::abs(signal_lab_test::correlate(second, tone_hz - realised, rate_hz));
        INFO("shift " << shift << " Hz");
        CHECK(std::abs(line.slope / two_pi) <= frequency_bound_hz);
        CHECK(signal_lab_test::amplitude_db(mirror / wanted) <= mirror_limit_db);
    }
}

TEST_CASE("watterson: the 48 kHz gain is the linear interpolation of the reported nodes, bit for bit (U13 b)", "[watterson]")
{
    auto channel = signal_lab_test::make_channel();
    const std::vector<float> input = tone_input(interpolation_frames, tone_hz);

    for (const double spread : interpolation_spreads_hz)
    {
        signal_lab_test::TapRecorder recorder;
        const std::array paths{rayleigh_path(0.0, spread)};
        REQUIRE(channel->configure(params_of(paths), seed_one, stage_zero, &recorder).is_ok());
        bool ok = false;
        const std::vector<float> output = signal_lab_test::run(*channel, input, engine_block, ok);
        REQUIRE(ok);
        INFO("d = " << spread << " Hz, M = " << channel->report().paths[0].frames_per_node.value());
        CHECK(first_difference(output, reference_output(*channel, recorder, input)) == output.size());
    }
}

TEST_CASE("watterson: the tap-rate images of the 48 kHz gain are at most -75 dBc (U13 c)", "[watterson][statistics]")
{
    auto channel = signal_lab_test::make_channel();
    signal_lab_test::TapRecorder recorder;
    const std::array paths{rayleigh_path(0.0, image_spread_hz)};
    REQUIRE(channel->configure(params_of(paths), seed_one, stage_zero, &recorder).is_ok());
    const std::size_t frames = image_segment * image_segments;
    const std::vector<float> silence(frames, 0.0F);
    bool ok = false;
    const std::vector<float> output = signal_lab_test::run(*channel, silence, engine_block, ok);
    REQUIRE(ok);
    const sl::WattersonPathReport& report = channel->report().paths[0];
    const std::vector<sl::ComplexFloat> gains = reference_gain(report, recorder.path(0U), frames);
    std::vector<Complex> sequence(frames);
    std::ranges::transform(gains, sequence.begin(), [](sl::ComplexFloat gain) { return Complex{gain.real, gain.imag}; });

    const std::vector<double> power = signal_lab_test::average_periodogram(sequence, image_segment, signal_lab_test::Window::hann);
    const double bin_hz = rate_hz / sl::as_double(image_segment);
    const double window_hz = image_window_sigmas * sl::doppler_sigma_per_spread * image_spread_hz;
    const auto band = [&power, bin_hz, window_hz](double centre_hz)
    {
        double sum = 0.0;

        for (std::size_t bin = 0U; bin < power.size(); ++bin)
        {
            const double frequency = signal_lab_test::signed_bin(bin, power.size()) * bin_hz;
            sum += std::abs(frequency - centre_hz) <= window_hz ? power[bin] : 0.0;
        }

        return sum;
    };

    const double lobe = band(0.0);
    const double node_rate = report.node_rate.in(units::Hz);

    for (const double order : image_orders)
    {
        INFO("image at " << order << " R");
        CHECK(signal_lab_test::power_db(band(order * node_rate) / lobe) <= image_limit_db);
    }
}

TEST_CASE("watterson: path powers are normalised to a total of 1, exactly (U15, U18 c)", "[watterson]")
{
    auto channel = signal_lab_test::make_channel();
    std::array<sl::WattersonPath, four_levels_db.size()> paths{};

    for (std::size_t index = 0U; index < paths.size(); ++index)
    {
        paths.at(index) = rayleigh_path(delay_in_ms(four_delays.at(index)), four_spreads_hz.at(index), four_levels_db.at(index));
    }

    REQUIRE(channel->configure(params_of(paths), seed_one, stage_zero, nullptr).is_ok());
    const sl::WattersonReport& report = channel->report();
    const double first = realised_power(report.paths[0]);
    double total = 0.0;

    for (std::size_t index = 0U; index < paths.size(); ++index)
    {
        const sl::WattersonPathReport& path = report.paths.at(index);
        const double ratio = std::pow(decade, (four_levels_db.at(index) - four_levels_db[0]) / signal_lab_test::power_db_per_decade);
        INFO("path " << index);
        CHECK_THAT(realised_power(path) / first, WithinRel(ratio, normalisation_bound));
        CHECK(path.interpolation_power < 1.0);
        total += realised_power(path);
    }

    CHECK_THAT(total, WithinAbs(1.0, normalisation_bound));
    // U18 (c): a_i^2 v_i = p_i for p_i = 1, 0.5 and 0.1.
    const std::array single{rayleigh_path(0.0, one_hertz)};
    REQUIRE(channel->configure(params_of(single), seed_one, stage_zero, nullptr).is_ok());
    CHECK_THAT(realised_power(channel->report().paths[0]), WithinAbs(1.0, normalisation_bound));
    const std::array pair{rayleigh_path(0.0, one_hertz), rayleigh_path(second_delay_ms, one_hertz)};
    REQUIRE(channel->configure(params_of(pair), seed_one, stage_zero, nullptr).is_ok());
    CHECK_THAT(realised_power(channel->report().paths[1]), WithinAbs(half_power, normalisation_bound));
    const std::array uneven{rayleigh_path(0.0, one_hertz), rayleigh_path(second_delay_ms, one_hertz, ninth_db)};
    REQUIRE(channel->configure(params_of(uneven), seed_one, stage_zero, nullptr).is_ok());
    CHECK_THAT(realised_power(channel->report().paths[0]), WithinAbs(tenth, normalisation_bound));
}

TEST_CASE("watterson: the realised interval mean of |h(n)|^2 is the interpolation-weight form (U18 b)", "[watterson]")
{
    auto channel = signal_lab_test::make_channel();
    signal_lab_test::TapRecorder recorder;
    const std::array paths{rayleigh_path(0.0, ten_hertz)};
    REQUIRE(channel->configure(params_of(paths), seed_one, stage_zero, &recorder).is_ok());
    const std::vector<float> silence(interpolation_frames, 0.0F);
    bool ok = false;
    const std::vector<float> output = signal_lab_test::run(*channel, silence, engine_block, ok);
    REQUIRE(ok);
    const sl::WattersonPathReport& report = channel->report().paths[0];
    const std::vector<sl::TapRecord> records = recorder.path(0U);
    const std::vector<sl::ComplexFloat> gains = reference_gain(report, records, interpolation_frames);
    const std::uint64_t frames = report.frames_per_node.value();
    double worst = 0.0;

    for (std::size_t node = 0U; (node + 1U) * frames <= interpolation_frames; ++node)
    {
        const Complex now{records[node].value.real, records[node].value.imag};
        const Complex next{records[node + 1U].value.real, records[node + 1U].value.imag};
        double realised = 0.0;
        double weighted = 0.0;

        for (std::uint64_t offset = 0U; offset < frames; ++offset)
        {
            const auto mu = sl::as_double(offset) / sl::as_double(frames);
            const sl::ComplexFloat gain = gains[node * frames + offset];
            realised += std::norm(Complex{gain.real, gain.imag});
            weighted += std::norm((1.0 - mu) * now + mu * next);
        }

        worst = std::max(worst, std::abs(realised / weighted - 1.0));
    }

    CHECK(worst <= interval_bound);
}

TEST_CASE("watterson: a fixed and a Rayleigh path at one delay compose a Rician tap, K = 10 dB (U19)", "[watterson][statistics]")
{
    auto channel = signal_lab_test::make_channel();
    signal_lab_test::TapRecorder recorder;
    const std::array paths{fixed_path(0.0), rayleigh_path(0.0, ten_hertz, -rician_k_db)};
    REQUIRE(channel->configure(params_of(paths), seed_one, stage_zero, &recorder).is_ok());
    const sl::WattersonReport& report = channel->report();
    CHECK_THAT(realised_power(report.paths[0]) / realised_power(report.paths[1]), WithinRel(std::pow(decade, rician_k_db / signal_lab_test::power_db_per_decade), normalisation_bound));
    const std::size_t frames = rician_nodes * report.paths[1].frames_per_node.value();
    std::vector<float> block(engine_block, 0.0F);

    for (std::size_t offset = 0U; offset < frames; offset += engine_block)
    {
        std::ranges::fill(block, 0.0F);
        REQUIRE(channel->process(block, units::SampleIndex{offset}).is_ok());
    }

    const sl::ComplexFloat fixed = recorder.path(0U).front().value;
    const std::vector<sl::TapRecord> nodes = recorder.path(1U);
    REQUIRE(nodes.size() >= rician_nodes);
    double second = 0.0;
    double fourth = 0.0;

    for (std::size_t node = 0U; node < rician_nodes; ++node)
    {
        const double power = std::norm(Complex{fixed.real, fixed.imag} + Complex{nodes[node].value.real, nodes[node].value.imag});
        second += power;
        fourth += power * power;
    }

    const auto count = sl::as_double(rician_nodes);
    const double m2 = second / count;
    const double m4 = fourth / count;
    const double line_power = std::sqrt(moment_two * m2 * m2 - m4);
    CHECK_THAT(signal_lab_test::power_db(line_power / (m2 - line_power)), WithinAbs(rician_k_db, rician_bound_db));
}

TEST_CASE("watterson: the same seed gives the same digests, another seed or stage others (U20)", "[watterson]")
{
    auto channel = signal_lab_test::make_channel();
    const sl::WattersonParams params = reference_params();
    const std::vector<float> input = noise_input(partition_frames);
    bool ok = false;
    REQUIRE(channel->configure(params, seed_one, stage_zero, nullptr).is_ok());
    const std::uint64_t first_parameters = channel->report().parameter_digest;
    const std::uint64_t first_output = digest_of(signal_lab_test::run(*channel, input, engine_block, ok));
    REQUIRE(channel->configure(params, seed_one, stage_zero, nullptr).is_ok());
    CHECK(channel->report().parameter_digest == first_parameters);
    CHECK(digest_of(signal_lab_test::run(*channel, input, engine_block, ok)) == first_output);
    REQUIRE(channel->configure(params, seed_two, stage_zero, nullptr).is_ok());
    CHECK(channel->report().parameter_digest != first_parameters);
    CHECK(digest_of(signal_lab_test::run(*channel, input, engine_block, ok)) != first_output);
    REQUIRE(channel->configure(params, seed_one, stage_zero + 1U, nullptr).is_ok());
    CHECK(digest_of(signal_lab_test::run(*channel, input, engine_block, ok)) != first_output);
    CHECK(ok);
}

TEST_CASE("watterson: every block partition gives the reference model's output, bit for bit (U21)", "[watterson]")
{
    auto channel = signal_lab_test::make_channel();
    signal_lab_test::TapRecorder recorder;
    const sl::WattersonParams params = reference_params();
    const std::vector<float> input = noise_input(partition_frames);
    REQUIRE(channel->configure(params, seed_one, stage_zero, &recorder).is_ok());
    bool ok = false;
    const std::vector<float> whole = signal_lab_test::run(*channel, input, engine_block, ok);
    REQUIRE(ok);
    REQUIRE(channel->report().paths[1].delay == sl::watterson_max_delay_frames);
    CHECK(first_difference(whole, reference_output(*channel, recorder, input)) == whole.size());

    for (const std::size_t block : partition_blocks)
    {
        REQUIRE(channel->configure(params, seed_one, stage_zero, nullptr).is_ok());
        const std::vector<float> output = signal_lab_test::run(*channel, input, block, ok);
        INFO("block " << block);
        CHECK(ok);
        CHECK(first_difference(output, whole) == whole.size());
    }

    sl::det::Pcg32 generator{partition_seed, 0U, 0U};
    std::vector<std::size_t> blocks(random_partition_blocks);

    for (std::size_t& block : blocks)
    {
        block = 1U + generator.next() % random_partition_limit;
    }

    REQUIRE(channel->configure(params, seed_one, stage_zero, nullptr).is_ok());
    const std::vector<float> random = signal_lab_test::run_partition(*channel, input, blocks, ok);
    CHECK(ok);
    CHECK(first_difference(random, whole) == whole.size());
}

TEST_CASE("watterson: a path's nodes do not change when another path's parameters change (U22)", "[watterson]")
{
    auto channel = signal_lab_test::make_channel();
    const std::vector<float> silence(interpolation_frames, 0.0F);
    const std::array first_setup{rayleigh_path(0.0, one_hertz), rayleigh_path(second_delay_ms, half_hertz)};
    const std::array second_setup{rayleigh_path(0.0, one_hertz), rayleigh_path(other_delay_ms, other_spread_hz, 0.0, other_shift_hz)};
    const std::array third_setup{rayleigh_path(0.0, one_hertz), fixed_path(other_delay_ms, 0.0, other_phase_degrees)};
    const std::array setups{params_of(first_setup), params_of(second_setup), params_of(third_setup)};
    std::vector<std::vector<sl::TapRecord>> runs;

    for (const sl::WattersonParams& setup : setups)
    {
        signal_lab_test::TapRecorder recorder;
        REQUIRE(channel->configure(setup, seed_one, stage_zero, &recorder).is_ok());
        bool ok = false;
        const std::vector<float> output = signal_lab_test::run(*channel, silence, engine_block, ok);
        REQUIRE(ok);
        runs.push_back(recorder.path(0U));
    }

    REQUIRE(runs[0].size() > 2U);

    for (std::size_t run = 1U; run < runs.size(); ++run)
    {
        REQUIRE(runs[run].size() == runs[0].size());
        std::size_t differing = 0U;

        for (std::size_t node = 0U; node < runs[0].size(); ++node)
        {
            const bool same = std::bit_cast<std::uint64_t>(runs[run][node].value) == std::bit_cast<std::uint64_t>(runs[0][node].value);
            differing += same ? 0U : 1U;
        }

        INFO("setup " << run);
        CHECK(differing == 0U);
    }
}

TEST_CASE("watterson: golden digests and first nodes at seed 1 pin the draw order (U23)", "[watterson]")
{
    auto channel = signal_lab_test::make_channel();
    signal_lab_test::TapRecorder recorder;
    REQUIRE(channel->configure(reference_params(), seed_one, stage_zero, &recorder).is_ok());
    bool ok = false;
    const std::vector<float> output = signal_lab_test::run(*channel, noise_input(partition_frames), engine_block, ok);
    REQUIRE(ok);
    const sl::TapRecord first = recorder.path(0U).front();
    INFO("parameter digest 0x" << std::hex << channel->report().parameter_digest << ", output digest 0x" << digest_of(output));
    INFO("first node " << std::hexfloat << first.value.real << ", " << first.value.imag);
    CHECK(channel->report().parameter_digest == golden_parameter_digest);
    CHECK(digest_of(output) == golden_output_digest);
    CHECK(first.value.real == golden_first_node_real);
    CHECK(first.value.imag == golden_first_node_imag);
}

TEST_CASE("watterson: a channel object filled with 0xFF before configure() gives the reference output (U24)", "[watterson]")
{
    auto fresh = signal_lab_test::make_channel();
    auto poisoned = signal_lab_test::make_channel();
    // Every byte 0xFF, as NOLOAD OCRAM may hold after a reset: bit_cast from a byte image.
    auto image = std::make_unique<std::array<std::byte, sizeof(sl::WattersonChannel)>>();
    image->fill(poison_byte);
    *poisoned = std::bit_cast<sl::WattersonChannel>(*image);
    const std::vector<float> input = noise_input(partition_frames);
    REQUIRE(fresh->configure(reference_params(), seed_one, stage_zero, nullptr).is_ok());
    REQUIRE(poisoned->configure(reference_params(), seed_one, stage_zero, nullptr).is_ok());
    bool ok = false;
    const std::vector<float> expected = signal_lab_test::run(*fresh, input, engine_block, ok);
    const std::vector<float> output = signal_lab_test::run(*poisoned, input, engine_block, ok);
    CHECK(ok);
    CHECK(poisoned->report().parameter_digest == fresh->report().parameter_digest);
    CHECK(first_difference(output, expected) == expected.size());
}

TEST_CASE("watterson: a non-contiguous block latches a fault until configure()", "[watterson]")
{
    auto channel = signal_lab_test::make_channel();
    std::vector<float> block(engine_block, 0.0F);
    CHECK_FALSE(static_channel().process(block, units::SampleIndex{}).is_ok());
    const std::array paths{rayleigh_path(0.0, one_hertz)};
    REQUIRE(channel->configure(params_of(paths), seed_one, stage_zero, nullptr).is_ok());
    CHECK(channel->process(block, units::SampleIndex{}).is_ok());
    CHECK_FALSE(channel->process(block, units::SampleIndex{engine_block + 1U}).is_ok());
    CHECK_FALSE(channel->process(block, units::SampleIndex{engine_block}).is_ok());
    REQUIRE(channel->configure(params_of(paths), seed_one, stage_zero, nullptr).is_ok());
    CHECK(channel->process(block, units::SampleIndex{}).is_ok());
}
