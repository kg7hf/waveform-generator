// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// Catch2 suite of the AWGN stage (signal_lab/awgn.hpp) and its typed engine slot:
// design 7.3, tests U29 to U33, U36's AWGN case, and the arena that shrank with it.
// Guide: docs/modules/signal-lab-engine.md, "AWGN".

#include "signal_lab/awgn.hpp"
#include "signal_lab/convert.hpp"
#include "signal_lab/det_gaussian.hpp"
#include "signal_lab/det_math.hpp"
#include "signal_lab/mixer.hpp"
#include "signal_lab/scenario.hpp"
#include "test_convert.hpp"
#include "test_spectrum.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <span>
#include <string_view>
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
constexpr double nyquist_scale = 2.0; // fs / 2, and N0 = 2 P for a real signal

// U29: flat within +/-0.02 dB over 300-3300 Hz (design 7.3 computes -0.009 / +0.010),
// unit gain at 1800 Hz, ENBW 3233.4 +/- 0.5 Hz; 583 taps.
constexpr double flatness_bound_db = 0.02;
constexpr double flatness_step_hz = 5.0;
constexpr double reference_gain_bound = 1.0e-6;
constexpr double expected_enbw_hz = 3233.4;
constexpr double enbw_bound_hz = 0.5;
constexpr std::uint32_t expected_taps = 583U;

// U30 to U32: one seeded render of 2^22 frames (87 s) of noise alone, at 10 dB against
// a reference RMS of 0.125 (-18 dBFS).
constexpr std::size_t render_frames = std::size_t{1} << 22U;
constexpr double reference_rms = 0.125;
constexpr double snr_db = 10.0;
constexpr std::size_t engine_block = 2048U;
constexpr std::string_view noise_scenario = R"({"seed": 7, "source_gain_db": 0, "impairments": [{"type": "awgn", "snr_db": 10}]})";
// U30: N0 x 3000 Hz equals the commanded noise power within 0.05 dB, by the Welch
// mean over 500-3100 Hz. 1024 unwindowed 4096-point segments x 222 bins give a
// relative s.e. of 1 / sqrt(227000), 0.009 dB, so 0.05 dB is 5.5 of them.
constexpr std::size_t welch_segment = 4096U;
constexpr double interior_low_hz = 500.0;
constexpr double interior_high_hz = 3100.0;
constexpr double n0_bound_db = 0.05;
// U31: excess kurtosis within 4 s.e. of 0; for a Gaussian process the s.e. is
// sqrt(24 sum_tau rho(tau)^4 / N), rho the kernel's normalised autocorrelation.
constexpr double kurtosis_variance_scale = 24.0;
constexpr double gaussian_kurtosis = 3.0;
constexpr double kurtosis_standard_errors = 4.0;
// sum over all lags = 2 sum over tau >= 0, less the rho(0)^4 = 1 counted twice.
constexpr double two_sided_lags = 2.0;
constexpr double decade = 10.0;
// U32: no in-band bin above the chi-square upper 1e-6 point over its local median:
// for the mean of 1024 chi-square(2) / 2 values (nu = 2048) Wilson-Hilferty gives 1.156.
constexpr double comb_ratio_limit = 1.156;
constexpr std::size_t median_halfwidth = 16U;

// U33: block partitions (design 10.1).
constexpr std::array partition_blocks{std::size_t{1}, std::size_t{63}, std::size_t{64}, std::size_t{65}, std::size_t{96}, std::size_t{448}, std::size_t{2048}};
constexpr std::size_t partition_frames = 20000U;
constexpr std::uint64_t partition_seed = 11U;
constexpr std::uint32_t partition_index = 3U;
constexpr double input_centre = 0.5;

// The window [start, start + duration) of the stage (resolve_window): an odd first
// frame and an odd length, so the gate never lines up with a Box-Muller pair. A
// shorter stream ends the window early; a live stream (total 0) has no end.
constexpr double window_start_seconds = 1001.0 / rate_hz;
constexpr double window_length_seconds = 9601.0 / rate_hz;
constexpr std::uint64_t window_start_frame = 1001U;
constexpr std::uint64_t window_end_frame = 10602U;
constexpr std::uint64_t live_stream = 0U;
constexpr std::uint64_t short_stream = 7000U;

// A sample slip ahead of the stage changes a block's frame count, while the next
// block still starts at the engine's input cursor (mixer.cpp). Input blocks of 1001
// frames: one frame dropped from blocks 0 and 2, or one duplicated in blocks 0 and 2.
struct SlipCall
{
    std::uint64_t first;
    std::size_t frames;
};

constexpr std::array dropped_calls{SlipCall{0U, 1000U}, SlipCall{1001U, 1001U}, SlipCall{2002U, 1000U}, SlipCall{3003U, 1001U}};
constexpr std::array duplicated_calls{SlipCall{0U, 1002U}, SlipCall{1001U, 1001U}, SlipCall{2002U, 1002U}, SlipCall{3003U, 1001U}};
// Those blocks end on an odd frame, so no carry is pending at the slip. Two frames
// dropped or duplicated end blocks 0 and 2 on an even frame: its pair's second value
// waits for the next odd frame (999 or 1003, 3001 or 3005), and the next block starts
// at another odd frame (1001, 3003), which must draw a fresh pair (QA on bad48c4).
constexpr std::array dropped_two_calls{SlipCall{0U, 999U}, SlipCall{1001U, 1001U}, SlipCall{2002U, 999U}, SlipCall{3003U, 1001U}};
constexpr std::array duplicated_two_calls{SlipCall{0U, 1003U}, SlipCall{1001U, 1001U}, SlipCall{2002U, 1003U}, SlipCall{3003U, 1001U}};
// The duplicate, then a block of one frame (two input frames, one dropped): frame 1001
// draws a fresh pair, and the carry block 0 left for 1003 is gone when block 2 starts there.
constexpr std::array duplicated_then_one_frame_calls{SlipCall{0U, 1003U}, SlipCall{1001U, 1U}, SlipCall{1003U, 1001U}};

// Representative impairment stage sequences: every one must fit the 28 KiB arena.
constexpr std::array representative_chains{
    std::string_view{R"({"impairments": [{"type": "cw", "frequency_hz": 1800, "ci_db": 6}]})"},
    std::string_view{R"({"impairments": [{"type": "impulse", "rate_per_sec": 1.0, "peak_db": 20, "decay_ms": 20}]})"},
    std::string_view{R"({"impairments": [{"type": "fade", "depth_db": 18, "duration_ms": 250, "period_seconds": 10}]})"},
    std::string_view{R"({"impairments": [{"type": "sample_slip", "kind": "delete", "length_samples": 48, "at_seconds": 1}]})"},
    std::string_view{R"({"impairments": [{"type": "awgn", "snr_db": 7}]})"},
    std::string_view{R"({"impairments": [{"type": "awgn", "snr_db": 7}, {"type": "cw", "frequency_hz": 1800, "ci_db": 3},)"
                     R"( {"type": "impulse", "rate_per_sec": 1.0, "peak_db": 20, "decay_ms": 20}]})"},
    std::string_view{R"({"impairments": [{"type": "fade", "depth_db": 24, "duration_ms": 250, "period_seconds": 10}, {"type": "awgn", "snr_db": 3},)"
                     R"( {"type": "cw", "frequency_hz": 1800, "ci_db": 3}]})"},
    std::string_view{R"({"impairments": [{"type": "awgn", "snr_db": 3}, {"type": "impulse", "rate_per_sec": 1.0, "peak_db": 20, "decay_ms": 20},)"
                     R"( {"type": "sample_slip", "kind": "delete", "length_samples": 5, "at_seconds": 1}]})"},
    std::string_view{R"({"impairments": [{"type": "fade", "depth_db": 24, "duration_ms": 250, "first_seconds": 10, "period_seconds": 10},)"
                     R"( {"type": "awgn", "snr_db": 3}, {"type": "cw", "frequency_hz": 1800, "ci_db": 3},)"
                     R"( {"type": "impulse", "rate_per_sec": 1.0, "peak_db": 20, "decay_ms": 20},)"
                     R"( {"type": "sample_slip", "kind": "delete", "length_samples": 5, "placement": "spaced", "first_seconds": 15, "period_seconds": 20, "count": 5}]})"},
};
constexpr std::uint64_t corpus_frames = 480000U;
constexpr std::string_view two_awgn = R"({"impairments": [{"type": "awgn", "snr_db": 20}, {"type": "awgn", "snr_db": 30}]})";
constexpr std::string_view three_awgn = R"({"impairments": [{"type": "awgn", "snr_db": 20}, {"type": "awgn", "snr_db": 30}, {"type": "awgn", "snr_db": 40}]})";
constexpr std::string_view stage_limit_message = "awgn: at most 2 stages per scenario";
// awgn_max_snr bounds a closed range: the limit itself is valid, and a step of 0.001 dB
// beyond it is not, in the stage's prepare() and in the scenario's validation.
constexpr double snr_limit_step_db = 0.001;
constexpr std::array snr_limit_signs{1.0, -1.0}; // +120 dB and -120 dB

struct SnrScenario
{
    std::string_view text;
    bool valid;
};

constexpr std::array snr_limit_scenarios{
    SnrScenario{R"({"impairments": [{"type": "awgn", "snr_db": 120}]})", true},
    SnrScenario{R"({"impairments": [{"type": "awgn", "snr_db": -120}]})", true},
    SnrScenario{R"({"impairments": [{"type": "awgn", "snr_db": 120.001}]})", false},
    SnrScenario{R"({"impairments": [{"type": "awgn", "snr_db": -120.001}]})", false},
};

// |H(f)|^2 of the stored kernel, in double.
[[nodiscard]] double kernel_power(const sl::NoiseBandKernel& kernel, double frequency)
{
    const std::span<const float> half{kernel.half};
    auto response = sl::as_double(half[sl::noise_band_centre]);

    for (std::uint32_t tap = 0U; tap < sl::noise_band_centre; ++tap)
    {
        const auto offset = sl::as_double(sl::noise_band_centre - tap);
        response += nyquist_scale * sl::as_double(half[tap]) * std::cos(two_pi * frequency * offset / rate_hz);
    }

    return response * response;
}

struct Rendered
{
    std::vector<float> output;
    bool ok{};
};

[[nodiscard]] Rendered render_noise(std::string_view text, std::size_t frames)
{
    auto scenario = std::make_unique<sl::Scenario>();
    auto engine = std::make_unique<sl::Engine>();
    Rendered rendered{};

    if (!sl::parse_scenario(text.data(), text.size(), *scenario).is_ok() || !engine->configure(*scenario, reference_rms, frames).is_ok())
    {
        return rendered;
    }

    const std::vector<float> silence(engine_block, 0.0F);
    std::vector<float> block(sl::engine_capacity_frames);
    rendered.output.reserve(frames);

    for (std::size_t done = 0U; done < frames; done += engine_block)
    {
        const m110::Result<std::size_t> produced = engine->process(silence, block);

        if (!produced.has_value() || produced.value() != engine_block)
        {
            return rendered;
        }

        std::ranges::copy(std::span<const float>{block}.first(produced.value()), std::back_inserter(rendered.output));
    }

    rendered.ok = true;
    return rendered;
}

// The kernel's normalised autocorrelation rho(tau), tau = 0 ... 582.
[[nodiscard]] std::vector<double> kernel_autocorrelation(const sl::NoiseBandKernel& kernel)
{
    std::vector<double> taps(sl::noise_band_taps);
    const std::span<const float> half{kernel.half};

    for (std::uint32_t index = 0U; index < sl::noise_band_taps; ++index)
    {
        taps[index] = sl::as_double(half[std::min(index, sl::noise_band_history - index)]);
    }

    std::vector<double> rho(sl::noise_band_taps, 0.0);

    for (std::size_t lag = 0U; lag < taps.size(); ++lag)
    {
        for (std::size_t index = 0U; index + lag < taps.size(); ++index)
        {
            rho[lag] += taps[index] * taps[index + lag];
        }
    }

    const double zero = rho.front();
    std::ranges::transform(rho, rho.begin(), [zero](double value) { return value / zero; });

    return rho;
}

[[nodiscard]] std::vector<float> uniform_input(std::size_t frames)
{
    sl::det::Pcg32 generator{partition_seed, 0U, 0U};
    std::vector<float> input(frames);

    for (float& sample : input)
    {
        sample = sl::to_float(generator.uniform() - input_centre);
    }

    return input;
}

[[nodiscard]] sl::AwgnParams partition_params()
{
    sl::AwgnParams params{};
    params.snr = units::Ratio{snr_db, units::dB};
    return params;
}

[[nodiscard]] sl::AwgnParams window_params(bool has_duration)
{
    sl::AwgnParams params = partition_params();
    params.window = sl::Window{window_start_seconds, has_duration, window_length_seconds};
    return params;
}

[[nodiscard]] std::vector<float> run_stage(std::span<const float> input, std::size_t block, const sl::AwgnParams& params, std::uint64_t total_frames)
{
    std::vector<float> scratch(sl::engine_capacity_frames);
    const sl::StageContext context{reference_rms, total_frames, partition_seed, partition_index, scratch.data()};
    auto stage = std::make_unique<sl::AwgnStage>(params);
    std::vector<float> output(input.begin(), input.end());

    if (!stage->prepare(context).is_ok())
    {
        return {};
    }

    for (std::size_t offset = 0U; offset < output.size(); offset += block)
    {
        const std::size_t count = std::min(block, output.size() - offset);
        const std::span<float> part = std::span{output}.subspan(offset, count);
        const m110::Result<std::size_t> processed = stage->process(part.data(), part.size(), offset, sl::engine_capacity_frames);

        if (!processed.has_value() || processed.value() != count)
        {
            return {};
        }
    }

    return output;
}

[[nodiscard]] std::vector<float> run_stage(std::span<const float> input, std::size_t block)
{
    return run_stage(input, block, partition_params(), input.size());
}

// Design 7.3 written out directly: the same pairs and taps, a direct-form convolution,
// no chunks and no history buffer.
[[nodiscard]] std::vector<float> reference_stage(std::span<const float> input)
{
    sl::det::Pcg32 generator{partition_seed, sl::awgn_rng_family, partition_index};
    std::vector<float> excitation;
    excitation.reserve(sl::noise_band_history + input.size() + 1U);

    while (excitation.size() < sl::noise_band_history + input.size())
    {
        const sl::det::GaussianPair pair = sl::det::draw_gaussian_pair(generator);
        excitation.push_back(sl::to_float(pair.first));
        excitation.push_back(sl::to_float(pair.second));
    }

    const std::span<const float> half{sl::noise_band_kernel().half};
    const float scale = sl::to_float(sl::awgn_noise_sigma(units::Amplitude{reference_rms}, units::Ratio{snr_db, units::dB}).full_scale());
    std::vector<float> output(input.begin(), input.end());

    for (std::size_t frame = 0U; frame < output.size(); ++frame)
    {
        float noise = 0.0F;

        for (std::size_t tap = 0U; tap < sl::noise_band_centre; ++tap)
        {
            noise += half[tap] * (excitation[frame + tap] + excitation[frame + sl::noise_band_history - tap]);
        }

        noise += half[sl::noise_band_centre] * excitation[frame + sl::noise_band_centre];
        output[frame] += noise * scale;
    }

    return output;
}

[[nodiscard]] bool same_bits(std::span<const float> first, std::span<const float> second)
{
    return std::ranges::equal(first, second, [](float a, float b) { return std::bit_cast<std::uint32_t>(a) == std::bit_cast<std::uint32_t>(b); });
}

[[nodiscard]] bool same_bits(float first, float second)
{
    return std::bit_cast<std::uint32_t>(first) == std::bit_cast<std::uint32_t>(second);
}

// The window's edges by name (QA on d896a99, issue #20): the first frame of the window
// has the noise and the one before it none; where the window ends inside the render, the
// last frame in it has the noise and the first frame after it none.
void check_window_edges(std::span<const float> output, std::span<const float> input, std::span<const float> noisy, std::uint64_t end_frame)
{
    const std::size_t first = sl::to_size(window_start_frame);
    CHECK(same_bits(output[first - 1U], input[first - 1U]));
    CHECK(same_bits(output[first], noisy[first]));
    const std::size_t end = sl::to_size(end_frame);

    if (end < output.size())
    {
        CHECK(same_bits(output[end - 1U], noisy[end - 1U]));
        CHECK(same_bits(output[end], input[end]));
    }
}

// Which odd frame a pending carry goes to: its own, as awgn.hpp's rule says, or the
// next odd frame the stage sees, whichever it is (the rule without its frame check).
enum class CarryRule : std::uint8_t
{
    own_frame,
    any_odd_frame,
};

// The excitation rule of awgn.hpp written out over the frames the calls name, in call
// order: an even frame draws a pair and keeps its second value for the next frame; an
// odd frame takes that value only if it is its own and unused, and draws a fresh pair
// otherwise. Then the direct-form FIR over the values in that order, on silence.
[[nodiscard]] std::vector<float> slipped_reference(std::span<const SlipCall> calls, CarryRule rule = CarryRule::own_frame)
{
    sl::det::Pcg32 generator{partition_seed, sl::awgn_rng_family, partition_index};
    std::vector<float> excitation;

    while (excitation.size() < sl::noise_band_history)
    {
        const sl::det::GaussianPair pair = sl::det::draw_gaussian_pair(generator);
        excitation.push_back(sl::to_float(pair.first));
        excitation.push_back(sl::to_float(pair.second));
    }

    bool pending = false;
    std::uint64_t carry_frame = 0U;
    float carry = 0.0F;

    for (const SlipCall& call : calls)
    {
        for (std::uint64_t frame = call.first; frame < call.first + call.frames; ++frame)
        {
            const bool odd = (frame & 1U) != 0U;

            if (odd && pending && (rule == CarryRule::any_odd_frame || carry_frame == frame))
            {
                excitation.push_back(carry);
                pending = false;
                continue;
            }

            const sl::det::GaussianPair pair = sl::det::draw_gaussian_pair(generator);
            excitation.push_back(sl::to_float(odd ? pair.second : pair.first));
            carry = sl::to_float(pair.second);
            carry_frame = frame + 1U;
            pending = !odd;
        }
    }

    const std::span<const float> half{sl::noise_band_kernel().half};
    const float scale = sl::to_float(sl::awgn_noise_sigma(units::Amplitude{reference_rms}, units::Ratio{snr_db, units::dB}).full_scale());
    std::vector<float> output(excitation.size() - sl::noise_band_history, 0.0F);

    for (std::size_t frame = 0U; frame < output.size(); ++frame)
    {
        float noise = 0.0F;

        for (std::size_t tap = 0U; tap < sl::noise_band_centre; ++tap)
        {
            noise += half[tap] * (excitation[frame + tap] + excitation[frame + sl::noise_band_history - tap]);
        }

        noise += half[sl::noise_band_centre] * excitation[frame + sl::noise_band_centre];
        output[frame] += noise * scale;
    }

    return output;
}

[[nodiscard]] std::vector<float> run_slipped(std::span<const SlipCall> calls)
{
    std::vector<float> scratch(sl::engine_capacity_frames);
    auto stage = std::make_unique<sl::AwgnStage>(partition_params());
    std::vector<float> output;

    if (!stage->prepare(sl::StageContext{reference_rms, live_stream, partition_seed, partition_index, scratch.data()}).is_ok())
    {
        return output;
    }

    for (const SlipCall& call : calls)
    {
        std::vector<float> block(call.frames, 0.0F);
        const m110::Result<std::size_t> processed = stage->process(block.data(), block.size(), call.first, sl::engine_capacity_frames);

        if (!processed.has_value() || processed.value() != block.size())
        {
            return {};
        }

        output.insert(output.end(), block.begin(), block.end());
    }

    return output;
}

} // namespace

TEST_CASE("awgn: the noise band is flat over 300-3300 Hz with unit gain at 1800 Hz and ENBW 3233.4 Hz (U29)", "[awgn]")
{
    const sl::NoiseBandKernel& kernel = sl::noise_band_kernel();
    CHECK(sl::noise_band_taps == expected_taps);
    CHECK_THAT(kernel_power(kernel, sl::noise_band_reference.in(units::Hz)), WithinAbs(1.0, reference_gain_bound));
    double lowest = 0.0;
    double highest = 0.0;
    const double low = sl::noise_band_low.in(units::Hz);
    const double high = sl::noise_band_high.in(units::Hz);

    for (double frequency = low; frequency <= high; frequency += flatness_step_hz)
    {
        const double level = signal_lab_test::power_db(kernel_power(kernel, frequency));
        lowest = std::min(lowest, level);
        highest = std::max(highest, level);
    }

    CHECK(lowest >= -flatness_bound_db);
    CHECK(highest <= flatness_bound_db);
    double energy = 0.0;
    const std::span<const float> half{kernel.half};

    for (std::uint32_t index = 0U; index < sl::noise_band_taps; ++index)
    {
        const auto tap = sl::as_double(half[std::min(index, sl::noise_band_history - index)]);
        energy += tap * tap;
    }

    CHECK_THAT(rate_hz / nyquist_scale * energy, WithinAbs(expected_enbw_hz, enbw_bound_hz));
    CHECK_THAT(kernel.enbw.in(units::Hz), WithinRel(rate_hz / nyquist_scale * energy, reference_gain_bound));
}

TEST_CASE("awgn: N0 x 3000 Hz, Gaussian and comb-free on a seeded 87 s render (U30, U31, U32)", "[awgn][statistics]")
{
    const Rendered rendered = render_noise(noise_scenario, render_frames);
    REQUIRE(rendered.ok);
    const std::vector<Complex> sequence(rendered.output.begin(), rendered.output.end());
    const std::vector<double> power = signal_lab_test::average_periodogram(sequence, welch_segment);
    const double bin_hz = rate_hz / sl::as_double(welch_segment);
    // U30: N0 = 2 E|X_k|^2 / (N fs) for a real sequence, averaged over the interior bins.
    double interior = 0.0;
    std::size_t bins = 0U;

    for (std::size_t bin = 0U; bin < welch_segment / 2U; ++bin)
    {
        const double frequency = sl::as_double(bin) * bin_hz;

        if (frequency >= interior_low_hz && frequency <= interior_high_hz)
        {
            interior += power[bin];
            ++bins;
        }
    }

    const double n0 = nyquist_scale * interior / sl::as_double(bins) / (sl::as_double(welch_segment) * rate_hz);
    const double commanded = reference_rms * reference_rms * std::pow(decade, -snr_db / signal_lab_test::power_db_per_decade);
    CHECK_THAT(signal_lab_test::power_db(n0 * sl::snr_reference_bandwidth.in(units::Hz) / commanded), WithinAbs(0.0, n0_bound_db));
    // U31: kurtosis of the output against the s.e. of a Gaussian process with this kernel.
    double second = 0.0;
    double fourth = 0.0;

    for (const float sample : rendered.output)
    {
        const double square = sl::as_double(sample) * sl::as_double(sample);
        second += square;
        fourth += square * square;
    }

    const auto count = sl::as_double(rendered.output.size());
    const double variance = second / count;
    double rho_fourth = 0.0;

    for (const double rho : kernel_autocorrelation(sl::noise_band_kernel()))
    {
        rho_fourth += rho * rho * rho * rho;
    }

    const double kurtosis_se = std::sqrt(kurtosis_variance_scale * (two_sided_lags * rho_fourth - 1.0) / count);
    CHECK_THAT(fourth / count / (variance * variance) - gaussian_kurtosis, WithinAbs(0.0, kurtosis_standard_errors * kurtosis_se));
    // U32: no in-band line or comb: every bin at most the 1e-6 point over its local median.
    std::size_t lines = 0U;

    for (std::size_t bin = median_halfwidth; bin + median_halfwidth < welch_segment / 2U; ++bin)
    {
        const double frequency = sl::as_double(bin) * bin_hz;

        if (frequency < sl::noise_band_low.in(units::Hz) || frequency > sl::noise_band_high.in(units::Hz))
        {
            continue;
        }

        const std::span<const double> around = std::span<const double>{power}.subspan(bin - median_halfwidth, 2U * median_halfwidth + 1U);
        std::vector<double> neighbours(around.begin(), around.end());
        std::ranges::nth_element(neighbours, std::next(neighbours.begin(), signal_lab_test::offset_of(median_halfwidth)));
        lines += power[bin] > comb_ratio_limit * neighbours[median_halfwidth] ? 1U : 0U;
    }

    CHECK(lines == 0U);
}

TEST_CASE("awgn: every block partition gives the direct-form reference, bit for bit (U33)", "[awgn]")
{
    const std::vector<float> input = uniform_input(partition_frames);
    const std::vector<float> expected = reference_stage(input);

    for (const std::size_t block : partition_blocks)
    {
        INFO("block " << block);
        const std::vector<float> output = run_stage(input, block);
        REQUIRE(output.size() == input.size());
        CHECK(same_bits(output, expected));
    }
}

TEST_CASE("awgn: a window adds the noise in [start, start + duration) only, frame for frame, at every partition", "[awgn]")
{
    REQUIRE(sl::det::seconds_to_frames(window_start_seconds, sl::sample_rate_hz) == window_start_frame);
    REQUIRE(window_start_frame + sl::det::seconds_to_frames(window_length_seconds, sl::sample_rate_hz) == window_end_frame);
    const std::vector<float> input = uniform_input(partition_frames);
    // The unwindowed stage: U33's reference, the same draws (the gate never skips one).
    const std::vector<float> noisy = reference_stage(input);
    struct WindowCase
    {
        bool has_duration;
        std::uint64_t total;
        std::uint64_t end;
    };

    constexpr std::array cases{
        WindowCase{true, partition_frames, window_end_frame},  // a known stream, a duration
        WindowCase{true, live_stream, window_end_frame},       // a live stream: the duration still ends it
        WindowCase{false, live_stream, partition_frames},      // a live stream, no duration: open to the end
        WindowCase{false, partition_frames, partition_frames}, // a known stream, no duration: to its end
        WindowCase{true, short_stream, short_stream},          // the stream ends before the window does
    };

    for (const WindowCase& window : cases)
    {
        for (const std::size_t block : partition_blocks)
        {
            INFO("duration " << window.has_duration << ", total " << window.total << ", block " << block);
            const std::vector<float> output = run_stage(input, block, window_params(window.has_duration), window.total);
            REQUIRE(output.size() == input.size());
            std::size_t wrong = 0U;

            for (std::size_t frame = 0U; frame < output.size(); ++frame)
            {
                const bool inside = frame >= window_start_frame && frame < window.end;
                wrong += same_bits(output[frame], inside ? noisy[frame] : input[frame]) ? 0U : 1U;
            }

            CHECK(wrong == 0U);
            check_window_edges(output, input, noisy, window.end);
        }
    }
}

TEST_CASE("awgn: after a sample slip no excitation value repeats, at odd block sizes", "[awgn]")
{
    for (const std::span<const SlipCall> calls : {std::span<const SlipCall>{dropped_calls}, std::span<const SlipCall>{duplicated_calls}})
    {
        const std::vector<float> expected = slipped_reference(calls);
        const std::vector<float> output = run_slipped(calls);
        INFO("first block " << calls.front().frames << " frames");
        REQUIRE(output.size() == expected.size());
        CHECK(same_bits(output, expected));
    }
}

TEST_CASE("awgn: a carry left pending by a block that ends on an even frame goes only to its own frame", "[awgn]")
{
    for (const std::span<const SlipCall> calls :
         {std::span<const SlipCall>{dropped_two_calls}, std::span<const SlipCall>{duplicated_two_calls}, std::span<const SlipCall>{duplicated_then_one_frame_calls}})
    {
        const std::vector<float> expected = slipped_reference(calls);
        const std::vector<float> output = run_slipped(calls);
        INFO("first block " << calls.front().frames << " frames, " << calls.size() << " blocks");
        REQUIRE(output.size() == expected.size());
        CHECK(same_bits(output, expected));
        // The case tells the rules apart: from the first frame after the slip (1001), which
        // takes a fresh pair's second value, the output is not the one a carry given to the
        // next odd frame would make.
        const std::size_t after_slip = calls.front().frames;
        const std::vector<float> carried = slipped_reference(calls, CarryRule::any_odd_frame);
        REQUIRE(carried.size() == output.size());
        CHECK(same_bits(std::span{output}.first(after_slip), std::span{carried}.first(after_slip)));
        CHECK_FALSE(same_bits(output[after_slip], carried[after_slip]));
    }
}

TEST_CASE("awgn: at most two stages, each in a typed slot outside the arena (U36)", "[awgn][engine]")
{
    auto scenario = std::make_unique<sl::Scenario>();
    auto engine = std::make_unique<sl::Engine>();
    REQUIRE(sl::parse_scenario(two_awgn.data(), two_awgn.size(), *scenario).is_ok());
    REQUIRE(engine->configure(*scenario, reference_rms, corpus_frames).is_ok());
    CHECK(engine->stats().arena_bytes_used == 0U);
    CHECK(engine->stage(0U) != nullptr);
    CHECK(engine->stage(1U) != nullptr);
    CHECK(engine->stage(0U)->type() == sl::StageType::awgn);
    CHECK(sl::impairment_size(sl::StageType::awgn) == 0U);
    const m110::Status three = sl::parse_scenario(three_awgn.data(), three_awgn.size(), *scenario);
    CHECK(three.code == m110::StatusCode::invalid_configuration);
    CHECK(std::string_view{three.message} == stage_limit_message);
}

TEST_CASE("awgn: representative stage chains fit the 28 KiB arena", "[awgn][engine]")
{
    for (const std::string_view chain : representative_chains)
    {
        auto scenario = std::make_unique<sl::Scenario>();
        auto engine = std::make_unique<sl::Engine>();
        INFO(chain);
        REQUIRE(sl::parse_scenario(chain.data(), chain.size(), *scenario).is_ok());
        CHECK(engine->configure(*scenario, reference_rms, corpus_frames).is_ok());
        CHECK(engine->stats().arena_bytes_used <= sl::impairment_arena_bytes);
    }
}

TEST_CASE("awgn: prepare refuses an SNR out of range and a missing scratch", "[awgn]")
{
    std::vector<float> scratch(sl::engine_capacity_frames);
    sl::AwgnParams params{};
    params.snr = units::Ratio{sl::awgn_max_snr.in(units::dB) + 1.0, units::dB};
    auto stage = std::make_unique<sl::AwgnStage>(params);
    const m110::Status snr = stage->prepare(sl::StageContext{reference_rms, 0U, 1U, 0U, scratch.data()});
    CHECK(snr.code == m110::StatusCode::invalid_configuration);
    params.snr = units::Ratio{snr_db, units::dB};
    stage = std::make_unique<sl::AwgnStage>(params);
    const m110::Status scratch_missing = stage->prepare(sl::StageContext{reference_rms, 0U, 1U, 0U, nullptr});
    CHECK(scratch_missing.code == m110::StatusCode::invalid_argument);
    CHECK(stage->prepare(sl::StageContext{reference_rms, 0U, 1U, 0U, scratch.data()}).is_ok());
}

TEST_CASE("awgn: the SNR range is closed: +/-120 dB is accepted, 0.001 dB beyond it refused", "[awgn]")
{
    std::vector<float> scratch(sl::engine_capacity_frames);
    const double limit = sl::awgn_max_snr.in(units::dB);

    for (const double sign : snr_limit_signs)
    {
        INFO("sign " << sign);
        sl::AwgnParams params{};
        params.snr = units::Ratio{sign * limit, units::dB};
        CHECK(std::make_unique<sl::AwgnStage>(params)->prepare(sl::StageContext{reference_rms, 0U, 1U, 0U, scratch.data()}).is_ok());
        params.snr = units::Ratio{sign * (limit + snr_limit_step_db), units::dB};
        CHECK_FALSE(std::make_unique<sl::AwgnStage>(params)->prepare(sl::StageContext{reference_rms, 0U, 1U, 0U, scratch.data()}).is_ok());
    }

    // The scenario's own validation holds the same closed range.
    for (const SnrScenario& item : snr_limit_scenarios)
    {
        auto scenario = std::make_unique<sl::Scenario>();
        INFO(item.text);
        CHECK(sl::parse_scenario(item.text.data(), item.text.size(), *scenario).is_ok() == item.valid);
    }
}
