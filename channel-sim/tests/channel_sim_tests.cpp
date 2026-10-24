// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// channel-sim unit tests (guide: docs/modules/pathsim.md).
//
//  - PCM16 conversions: pathsim's truncating writer, the lane resampler's rounding one.
//  - LaneResampler against a direct transcription of the lane tool's resample()
//    loop, bit for bit, both directions, for lengths around the filter's edges and
//    for any input chunking.
//  - LaneResampler refuses inf and NaN input (its fast path needs finite samples).
//  - ChannelSpec: pathsim main.cpp's option semantics and the range checks.
//  - pathsim's draw order: the first Rayleigh tap and the first AWGN pair for a
//    seed, against the UCRT stream assigned in the lane binary's order (the first
//    draw of each pair is the imaginary part). Portable, so it checks the order
//    under any compiler; arm-none-eabi-g++ would swap an unsequenced pair.
//  - PathsimDriver: same seed -> same bytes, another seed -> other bytes, any source
//    chunking -> same bytes, length preserved, the C runtime's rand untouched.
//  - LaneChain: equal to oracle resampler -> driver -> oracle resampler; a non-finite
//    input sample fails the job.
// The gates (channel-sim/gates/run_gates.py) compare with the lane's binaries.

#include "core/channel_spec.hpp"
#include "core/convert.hpp"
#include "core/lane_chain.hpp"
#include "core/lane_resampler.hpp"
#include "core/pathsim_driver.hpp"
#include "core/pcm.hpp"
#include "rng/pathsim_rng.hpp"
#include "rng/ucrt_lcg.hpp"

#include <GaussFIR.h>
#include <NoiseGen.h>
#include <Path.h>
#include <cmplx.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <numbers>
#include <span>
#include <vector>

namespace
{
namespace cs = channel_sim;
using m110::Status;

// The lane chain's rates and the lane tool's constants.
constexpr std::uint32_t wide_hz = 48000U;
constexpr std::uint32_t narrow_hz = 8000U;
constexpr double near_zero = 1.0e-12;
constexpr double pcm16_output_scale = 32767.0;
constexpr cs::Decibels campaign_gain{-12.0};

// PCM16 boundaries.
constexpr cs::Pcm16 pcm16_lowest = -32768;
constexpr cs::Pcm16 pcm16_highest = 32767;
constexpr cs::Pcm16 pcm16_half_scale = 16384;
constexpr double half = 0.5;
constexpr double beyond_full_scale = 1.5;
constexpr double far_beyond_full_scale = 2.0;
constexpr double just_below_one_lsb = 0.99999;
constexpr double just_below_two_lsb = 1.99999;

// Test signals and jobs.
constexpr std::size_t odd_wide_length = 63603U; // 63603 * 8000 / 48000 = 10600.5
constexpr std::size_t odd_narrow_count = 10601U;
constexpr std::size_t read_chunk = 100U;
constexpr std::size_t upsampler_first_piece = 2999U;
constexpr std::size_t upsampler_second_piece_end = 3000U;
constexpr std::size_t driver_length = 10000U; // 5 blocks, the last one partial
constexpr std::uint64_t driver_blocks = 5U;
constexpr std::uint64_t mostly_active = 9000U;
constexpr std::size_t whole_blocks = 3U;
constexpr unsigned crt_seed = 4242U;
constexpr std::uint32_t golden_seed = 60001U;
constexpr std::uint32_t other_seed = 60002U;
constexpr cs::Hertz one_hertz{1.0};
constexpr cs::Milliseconds two_ms{2.0};
constexpr cs::Milliseconds sixty_ms{60.0};
constexpr cs::Milliseconds longest_delay_step{305.875}; // lag 2447, the ring's last entry
constexpr cs::Decibels golden_snr{17.6};
constexpr std::size_t inf_position = 3006U; // 6 * 485 + 96: an interior output's skipped tap

// pathsim's draw-order constants (Path.cpp, NoiseGen.cpp).
constexpr double draw_tolerance = 1.0e-9;
constexpr double noise_rms = 0.25;
constexpr double noise_enbw = 1.10; // NoiseGen.cpp K_ENBW
constexpr int noise_pair = 2;
constexpr double rayleigh_spread_hz = 1.0; // 0.4 < spread <= 2: the 64 Hz rate
constexpr double rayleigh_rate_hz = 64.0;
constexpr double gaussian_noise_bandwidth = 0.62665707; // Path.cpp KGNB
constexpr double rayleigh_gain_divisor = 4.0;
constexpr int rayleigh_preload = 250;    // Rayleigh::init's filter preload
constexpr double rsample_span = 2.0;     // rsample(): 1 - 2 rand() / RAND_MAX
constexpr double polar_log_scale = -2.0; // Marsaglia: sqrt(-2 ln(r2) / r2)

class Checks
{
public:
    void expect(bool condition, const char* what) noexcept
    {
        if (!condition)
        {
            ++failures_;
            std::fputs("FAIL: ", stdout);
            std::fputs(what, stdout);
            std::fputs("\n", stdout);
        }
    }

    [[nodiscard]] int failures() const noexcept { return failures_; }

private:
    int failures_{};
};

// Deterministic test signals from the UCRT generator: uniform in [-amplitude, amplitude].
[[nodiscard]] std::vector<double> noise(std::size_t count, double amplitude, std::uint32_t seed)
{
    constexpr double half_range = 16383.5;
    cs::UcrtLcg lcg{seed};
    std::vector<double> samples(count);
    std::ranges::generate(samples, [&lcg, amplitude]() noexcept { return amplitude * (static_cast<double>(lcg.next()) - half_range) / half_range; });

    return samples;
}

[[nodiscard]] std::vector<cs::Pcm16> to_pcm16(std::span<const double> samples)
{
    std::vector<cs::Pcm16> pcm(samples.size());
    std::ranges::transform(samples, pcm.begin(), cs::unit_to_pcm16_rounded);
    return pcm;
}

// The lane tool's resample() + gain + write_pcm16_wav quantization, transcribed
// directly (compliance/app_decode/wav_resample.cpp): the oracle.
[[nodiscard]] double oracle_sinc(double value) noexcept
{
    constexpr double pi = std::numbers::pi; // the tool's 3.1415926535897932384626433832795
    return std::abs(value) < near_zero ? 1.0 : std::sin(pi * value) / (pi * value);
}

[[nodiscard]] std::vector<cs::Pcm16> oracle_resample(std::span<const double> input, std::uint32_t input_rate_hz, std::uint32_t output_rate_hz, double gain)
{
    constexpr double pi = std::numbers::pi;
    constexpr std::int64_t radius = 96;
    const auto size = static_cast<std::int64_t>(input.size());
    const auto output_count = static_cast<std::size_t>(std::llround(static_cast<double>(input.size()) * output_rate_hz / input_rate_hz));
    const auto input_per_output = static_cast<double>(input_rate_hz) / output_rate_hz;
    const auto cutoff = 0.45 * std::min(1.0, static_cast<double>(output_rate_hz) / input_rate_hz);
    std::vector<cs::Pcm16> output(output_count);

    for (std::size_t output_index = 0U; output_index < output_count; ++output_index)
    {
        const auto source_position = static_cast<double>(output_index) * input_per_output;
        const auto center = static_cast<std::int64_t>(std::floor(source_position));
        double weighted_sum{};
        double weight_sum{};

        for (std::int64_t tap = -radius + 1; tap <= radius; ++tap)
        {
            const auto source_index = center + tap;

            if (source_index < 0 || source_index >= size)
            {
                continue;
            }

            const auto distance = source_position - static_cast<double>(source_index);
            const auto normalized_distance = distance / static_cast<double>(radius);

            if (std::abs(normalized_distance) >= 1.0)
            {
                continue;
            }

            const auto window = 0.42 + 0.5 * std::cos(pi * normalized_distance) + 0.08 * std::cos(2.0 * pi * normalized_distance);
            const auto weight = 2.0 * cutoff * oracle_sinc(2.0 * cutoff * distance) * window;
            weighted_sum += input[static_cast<std::size_t>(source_index)] * weight;
            weight_sum += weight;
        }

        auto sample = std::abs(weight_sum) > near_zero ? weighted_sum / weight_sum : 0.0;
        sample *= gain;
        const auto scaled = std::clamp(sample * 32767.0, -32768.0, 32767.0);
        output[output_index] = static_cast<cs::Pcm16>(std::lround(scaled));
    }

    return output;
}

// Streams a vector, in chunks of the given sizes (cycled).
template <typename Sample> class VectorSource final : public cs::SampleSource<Sample>
{
public:
    VectorSource(std::span<const Sample> samples, std::span<const std::size_t> chunks) noexcept : samples_{samples}, chunks_{chunks} {}

    [[nodiscard]] m110::Result<std::size_t> read(std::span<Sample> destination) noexcept override
    {
        const std::size_t chunk = chunks_[reads_ % chunks_.size()];
        ++reads_;
        const std::size_t count = std::min({destination.size(), chunk, samples_.size() - position_});
        std::ranges::copy(samples_.subspan(position_, count), destination.begin());
        position_ += count;
        return count;
    }

    [[nodiscard]] Status rewind() noexcept override
    {
        position_ = 0U;
        ++rewinds_;
        return Status::success();
    }

    [[nodiscard]] std::size_t rewinds() const noexcept { return rewinds_; }

private:
    std::span<const Sample> samples_;
    std::span<const std::size_t> chunks_;
    std::size_t position_{};
    std::size_t reads_{};
    std::size_t rewinds_{};
};

class VectorSink final : public cs::Pcm16Sink
{
public:
    [[nodiscard]] Status write(std::span<const cs::Pcm16> samples) noexcept override
    {
        samples_.insert(samples_.end(), samples.begin(), samples.end());
        return Status::success();
    }

    [[nodiscard]] const std::vector<cs::Pcm16>& samples() const noexcept { return samples_; }

private:
    std::vector<cs::Pcm16> samples_;
};

constexpr std::array<std::size_t, 1U> whole_chunks{4096U};
constexpr std::array<std::size_t, 5U> odd_chunks{1U, 7U, 2048U, 333U, 2U};

// LaneResampler, fed one sample at a time in stream order.
[[nodiscard]] std::vector<cs::Pcm16> stream_resample(std::span<const double> input, cs::SampleRate in, cs::SampleRate out, double gain, std::uint64_t* misses = nullptr)
{
    static cs::LaneResampler resampler;
    std::vector<cs::Pcm16> output;

    if (!resampler.configure(in, out, gain).is_ok())
    {
        return output;
    }

    const auto take_ready = [&output]()
    {
        while (resampler.ready())
        {
            output.push_back(resampler.next().value());
        }
    };

    for (const double sample : input)
    {
        take_ready();

        if (!resampler.push(sample).is_ok())
        {
            return {};
        }
    }

    resampler.finish();
    take_ready();

    if (misses != nullptr)
    {
        *misses = resampler.kernel_misses();
    }

    return output;
}

void test_pcm(Checks& checks)
{
    checks.expect(cs::pcm16_to_unit(pcm16_lowest) == -1.0, "PCM16 -32768 reads as -1");
    checks.expect(cs::pcm16_to_unit(pcm16_half_scale) == half, "PCM16 16384 reads as 0.5");
    checks.expect(cs::unit_to_pcm16_truncated(beyond_full_scale) == pcm16_highest, "pathsim writer clamps at +1 -> 32767");
    checks.expect(cs::unit_to_pcm16_truncated(-beyond_full_scale) == -pcm16_highest, "pathsim writer clamps at -1 -> -32767");
    checks.expect(cs::unit_to_pcm16_truncated(just_below_one_lsb / pcm16_output_scale) == 0, "pathsim writer truncates toward zero (+)");
    checks.expect(cs::unit_to_pcm16_truncated(-just_below_two_lsb / pcm16_output_scale) == -1, "pathsim writer truncates toward zero (-)");
    checks.expect(cs::unit_to_pcm16_truncated(std::nan("")) == 0, "pathsim writer maps NaN to 0");
    checks.expect(cs::unit_to_pcm16_rounded(half / pcm16_output_scale) == 1, "resampler writer rounds half away from zero (+)");
    checks.expect(cs::unit_to_pcm16_rounded(-half / pcm16_output_scale) == -1, "resampler writer rounds half away from zero (-)");
    checks.expect(cs::unit_to_pcm16_rounded(-far_beyond_full_scale) == pcm16_lowest, "resampler writer clamps at -32768");
    checks.expect(cs::unit_to_pcm16_rounded(far_beyond_full_scale) == pcm16_highest, "resampler writer clamps at 32767");
    checks.expect(cs::unit_to_pcm16_rounded(std::nan("")) == 0, "resampler writer maps NaN to 0");
}

void test_resampler(Checks& checks)
{
    constexpr std::array<std::size_t, 13U> lengths{1U, 2U, 5U, 95U, 96U, 97U, 191U, 192U, 193U, 194U, 1000U, 6001U, 20003U};
    const double gain = cs::amplitude_from_db(campaign_gain);
    bool down_exact = true;
    bool up_exact = true;

    for (const std::size_t length : lengths)
    {
        const std::vector<double> wide = noise(length * 6U, 0.5, static_cast<std::uint32_t>(length));
        down_exact = down_exact && stream_resample(wide, cs::lane_sample_rate, cs::pathsim_sample_rate, gain) == oracle_resample(wide, wide_hz, narrow_hz, gain);
        const std::vector<double> narrow = noise(length, 0.9, static_cast<std::uint32_t>(length + 1U));
        up_exact = up_exact && stream_resample(narrow, cs::pathsim_sample_rate, cs::lane_sample_rate, 1.0) == oracle_resample(narrow, narrow_hz, wide_hz, 1.0);
    }

    checks.expect(down_exact, "48 -> 8 kHz equals the lane tool's loop bit for bit");
    checks.expect(up_exact, "8 -> 48 kHz equals the lane tool's loop bit for bit");
    // Odd input lengths: llround(N * out / in) rounds half away from zero.
    const std::vector<double> odd = noise(odd_wide_length, half, 9U);
    checks.expect(stream_resample(odd, cs::lane_sample_rate, cs::pathsim_sample_rate, 1.0).size() == odd_narrow_count, "63603 -> llround(10600.5) = 10601");
    std::uint64_t misses = 0U;
    const std::vector<double> long_narrow = noise(80000U, 0.5, 3U);
    static_cast<void>(stream_resample(long_narrow, cs::pathsim_sample_rate, cs::lane_sample_rate, 1.0, &misses));
    // 480,000 outputs over six phases: the kernels are computed a handful of times, not per output.
    constexpr std::uint64_t reuse_bound = 1000U;
    checks.expect(misses > 0U && misses < reuse_bound, "8 -> 48 kHz kernels are reused");
    static cs::LaneResampler finite_only;
    checks.expect(finite_only.configure(cs::lane_sample_rate, cs::pathsim_sample_rate, 1.0).is_ok(), "resampler configures");
    checks.expect(!finite_only.push(std::numeric_limits<double>::infinity()).is_ok() && !finite_only.push(-std::numeric_limits<double>::infinity()).is_ok() && !finite_only.push(std::nan("")).is_ok(),
                  "the resampler refuses inf and NaN input");
    checks.expect(finite_only.push(half).is_ok() && finite_only.inputs() == 1U, "the resampler still takes finite input after a refusal");
}

void test_resampler_streams(Checks& checks)
{
    const std::vector<double> wide = noise(30001U, 0.4, 77U);
    const double gain = cs::amplitude_from_db(campaign_gain);
    const std::vector<cs::Pcm16> expected = oracle_resample(wide, wide_hz, narrow_hz, gain);
    VectorSource<double> source{wide, odd_chunks};
    static cs::LaneDownsampler downsampler;
    checks.expect(downsampler.attach(source, cs::lane_sample_rate, cs::pathsim_sample_rate, gain).is_ok(), "downsampler attaches");
    std::vector<cs::Pcm16> got;
    std::array<cs::Pcm16, read_chunk> chunk{};

    for (int pass = 0; pass < 2; ++pass)
    {
        got.clear();

        for (;;)
        {
            const m110::Result<std::size_t> count = downsampler.read(chunk);

            if (!count || count.value() == 0U)
            {
                break;
            }

            got.insert(got.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(count.value()));
        }

        checks.expect(got == expected, "the downsampler stream equals the lane tool's loop");
        checks.expect(downsampler.rewind().is_ok(), "the downsampler rewinds");
    }

    const std::vector<cs::Pcm16> narrow_pcm = to_pcm16(noise(5003U, 0.3, 78U));
    std::vector<double> narrow(narrow_pcm.size());
    std::ranges::transform(narrow_pcm, narrow.begin(), cs::pcm16_to_unit);
    VectorSink sink;
    static cs::LaneUpsampler upsampler;
    checks.expect(upsampler.attach(sink, cs::pathsim_sample_rate, cs::lane_sample_rate, 1.0).is_ok(), "upsampler attaches");
    const std::span<const cs::Pcm16> all{narrow_pcm};
    checks.expect(upsampler.write(all.first(1U)).is_ok() && upsampler.write(all.subspan(1U, upsampler_first_piece)).is_ok() && upsampler.write(all.subspan(upsampler_second_piece_end)).is_ok() &&
                      upsampler.finish().is_ok(),
                  "the upsampler takes pieces");
    checks.expect(sink.samples() == oracle_resample(narrow, narrow_hz, wide_hz, 1.0), "the upsampler stream equals the lane tool's loop");
}

void test_channel_spec(Checks& checks)
{
    cs::ChannelSpec poor;
    checks.expect(poor.apply_preset("ccir-poor").is_ok() && poor.paths().size() == 2U, "ccir-poor has two paths");
    constexpr cs::Hertz ccir_poor_spread{0.5};
    checks.expect(poor.paths()[1].delay.value == two_ms.value && poor.paths()[1].spread.value == ccir_poor_spread.value, "ccir-poor path 2: 2 ms, 0.5 Hz");
    checks.expect(!poor.noise().enabled, "presets other than awgn10 carry no AWGN");
    checks.expect(!poor.apply_preset("direct").is_ok(), "a second preset is refused");
    cs::ChannelSpec awgn;
    constexpr cs::Decibels awgn10_snr{10.0};
    checks.expect(awgn.apply_preset("awgn10").is_ok() && awgn.noise().enabled && awgn.noise().snr.value == awgn10_snr.value && awgn.paths().empty(), "awgn10 is AWGN at 10 dB, direct path");
    cs::ChannelSpec slots;
    checks.expect(slots.set_spread(2U, one_hertz).is_ok() && slots.paths().size() == 3U, "addressing path 3 adds paths 1 and 2");
    checks.expect(slots.paths()[0].spread.value == 0.0 && slots.paths()[1].delay.value == 0.0, "added paths are default (static, 0 ms)");
    checks.expect(!slots.apply_preset("ccir-poor").is_ok(), "a preset after other settings is refused");
    checks.expect(!slots.set_delay(0U, two_ms).is_ok(), "path 1 has no delay");
    checks.expect(!slots.set_spread(3U, one_hertz).is_ok(), "there is no path 4");
    constexpr cs::Milliseconds one_step{0.125}; // one 8 kHz sample
    cs::ChannelSpec bad;
    checks.expect(bad.set_spread(0U, cs::Hertz{cs::max_spread.value + half}).is_ok() && !bad.validate().is_ok(), "spread above 30 Hz is refused");
    cs::ChannelSpec far;
    checks.expect(far.set_delay(1U, cs::path_delay_limit).is_ok() && !far.validate().is_ok(), "a 306 ms delay (lag 2448, the delay ring's length) is refused");
    cs::ChannelSpec negative;
    checks.expect(negative.set_delay(1U, cs::Milliseconds{-one_step.value}).is_ok() && !negative.validate().is_ok(), "negative delay is refused");
    cs::ChannelSpec sixty;
    checks.expect(sixty.set_delay(1U, sixty_ms).is_ok() && sixty.validate().is_ok(), "a 60 ms delay is accepted, as pathsim.exe runs it");
    cs::ChannelSpec edge;
    checks.expect(edge.set_delay(1U, longest_delay_step).is_ok() && edge.validate().is_ok(), "a 305.875 ms delay (lag 2447) is accepted");
    cs::ChannelSpec just_below;
    checks.expect(just_below.set_delay(1U, cs::Milliseconds{std::nextafter(cs::path_delay_limit.value, 0.0)}).is_ok() && just_below.validate().is_ok(),
                  "a delay just below 306 ms is accepted (lag 2447)");
    cs::ChannelSpec loud;
    loud.set_snr(cs::Decibels{std::nan("")});
    checks.expect(!loud.validate().is_ok(), "a NaN SNR is refused");
    checks.expect(!cs::ChannelSpec{}.apply_preset("no-such-preset").is_ok(), "an unknown preset is refused");
}

[[nodiscard]] bool near(double got, double expected) noexcept
{
    return std::abs(got - expected) <= draw_tolerance * std::max(1.0, std::abs(expected));
}

// One of pathsim's rsample() draws: 1 - 2 rand() / RAND_MAX.
[[nodiscard]] double unit_draw(cs::UcrtLcg& lcg) noexcept
{
    return 1.0 - rsample_span * cs::as_double(lcg.next()) / cs::as_double(cs::ucrt_rand_max);
}

struct UnitPair
{
    double real{};
    double imaginary{};
    double radius_squared{};
};

// pathsim's draw loop (a pair inside the unit circle), in the lane binary's order:
// the first draw is the imaginary part.
[[nodiscard]] UnitPair lane_pair(cs::UcrtLcg& lcg) noexcept
{
    for (;;)
    {
        const double imaginary = unit_draw(lcg);
        const double real = unit_draw(lcg);
        const double radius_squared = real * real + imaginary * imaginary;

        if (radius_squared < 1.0 && radius_squared != 0.0)
        {
            return {real, imaginary, radius_squared};
        }
    }
}

// The Marsaglia polar factor that turns the pair into complex Gaussian noise.
[[nodiscard]] double polar_factor(const UnitPair& pair) noexcept
{
    return std::sqrt(polar_log_scale * std::log(pair.radius_squared) / pair.radius_squared);
}

void test_draw_order(Checks& checks)
{
    // AWGN (NoiseGen.cpp): with no band limit and a zero signal, the output is the noise pair.
    PathSim::NoiseGen generator{};
    generator.init(false);
    std::array<double, 2U> noise{};
    cs::seed_pathsim_rng(cs::Seed{golden_seed});
    generator.add_band_limited_noise(noise_pair, noise.data(), 0.0, noise_rms);
    cs::UcrtLcg noise_draws{golden_seed};
    const UnitPair pair = lane_pair(noise_draws);
    const double scale = noise_rms * noise_enbw * polar_factor(pair);
    checks.expect(near(noise[0], pair.real * scale) && near(noise[1], pair.imaginary * scale), "AWGN: the first draw of each pair is the imaginary part (the lane's order)");
    checks.expect(!near(noise[0], pair.imaginary * scale), "the AWGN check tells the two orders apart");
    // Rayleigh fading (Path.cpp): the first tap after init's 250-sample filter preload.
    PathSim::Rayleigh rayleigh{};
    cs::seed_pathsim_rng(cs::Seed{golden_seed});
    rayleigh.init(rayleigh_spread_hz, 1.0);
    const PathSim::cmplx tap = rayleigh.sample();
    const double gain = std::sqrt(rayleigh_rate_hz / (rayleigh_gain_divisor * rayleigh_spread_hz * gaussian_noise_bandwidth));
    PathSim::GaussFIR lane_order{};
    PathSim::GaussFIR swapped_order{};
    lane_order.init(rayleigh_rate_hz, rayleigh_spread_hz);
    swapped_order.init(rayleigh_rate_hz, rayleigh_spread_hz);
    cs::UcrtLcg tap_draws{golden_seed};
    PathSim::cmplx expected{};
    PathSim::cmplx swapped{};

    for (int sample = 0; sample <= rayleigh_preload; ++sample)
    {
        const UnitPair draw = lane_pair(tap_draws);
        const double factor = gain * polar_factor(draw);
        expected = lane_order.apply(PathSim::cmplx{draw.real * factor, draw.imaginary * factor});
        swapped = swapped_order.apply(PathSim::cmplx{draw.imaginary * factor, draw.real * factor});
    }

    checks.expect(near(tap.r, expected.r) && near(tap.i, expected.i), "Rayleigh: the first draw of each pair is the imaginary part (the lane's order)");
    checks.expect(!near(tap.r, swapped.r) || !near(tap.i, swapped.i), "the Rayleigh check tells the two orders apart");
}

[[nodiscard]] cs::PathsimJob two_path_job(std::uint32_t seed)
{
    cs::PathsimJob job{};
    static_cast<void>(job.channel.set_spread(0U, one_hertz));
    static_cast<void>(job.channel.set_delay(1U, two_ms));
    static_cast<void>(job.channel.set_spread(1U, one_hertz));
    job.channel.set_snr(golden_snr);
    job.seed = cs::Seed{seed};
    return job;
}

[[nodiscard]] std::vector<cs::Pcm16> run_driver(const cs::PathsimJob& job, std::span<const cs::Pcm16> input, std::span<const std::size_t> chunks, cs::PathsimRunReport* report = nullptr)
{
    static cs::PathsimDriver driver;
    VectorSource<cs::Pcm16> source{input, chunks};
    VectorSink sink;
    const m110::Result<cs::PathsimRunReport> result = driver.run(job, source, sink);

    if (!result)
    {
        return {};
    }

    if (report != nullptr)
    {
        *report = result.value();
    }

    return sink.samples();
}

void test_driver(Checks& checks)
{
    const std::vector<cs::Pcm16> input = to_pcm16(noise(driver_length, 0.06, 5U));
    cs::PathsimRunReport report{};
    std::srand(crt_seed);
    const int crt_expected = std::rand();
    std::srand(crt_seed);
    const std::vector<cs::Pcm16> first = run_driver(two_path_job(golden_seed), input, whole_chunks, &report);
    checks.expect(std::rand() == crt_expected, "pathsim's draws leave the C runtime's rand untouched");
    checks.expect(first.size() == input.size(), "output length equals input length");
    checks.expect(report.blocks == driver_blocks && report.samples == driver_length, "10000 samples are 5 blocks");
    checks.expect(report.active_samples > mostly_active && report.seed_rms > 0.0, "the level seed counts active samples");
    checks.expect(run_driver(two_path_job(golden_seed), input, whole_chunks) == first, "the same seed gives the same output");
    checks.expect(run_driver(two_path_job(golden_seed), input, odd_chunks) == first, "source chunking does not change the output");
    checks.expect(run_driver(two_path_job(other_seed), input, whole_chunks) != first, "another seed gives another output");
    const std::vector<cs::Pcm16> exact_blocks = to_pcm16(noise(whole_blocks * cs::PathsimDriver::block_samples, 0.06, 6U));
    checks.expect(run_driver(two_path_job(1U), exact_blocks, odd_chunks, &report).size() == exact_blocks.size() && report.blocks == whole_blocks, "a whole number of blocks needs no padding block");
    const std::vector<cs::Pcm16> silence(3000U, 0);
    checks.expect(run_driver(two_path_job(1U), silence, whole_chunks, &report).size() == silence.size() && report.active_samples == 0U && report.seed_rms == 0.0,
                  "silence leaves the level tracker unseeded");
    const std::vector<cs::Pcm16> empty;
    checks.expect(run_driver(two_path_job(1U), empty, whole_chunks, &report).empty() && report.blocks == 0U, "an empty input gives an empty output");
    cs::PathsimJob far = two_path_job(1U);
    checks.expect(far.channel.set_delay(1U, longest_delay_step).is_ok() && run_driver(far, input, odd_chunks).size() == input.size(), "the longest delay (lag 2447) runs");
    cs::PathsimJob invalid = two_path_job(1U);
    static_cast<void>(invalid.channel.set_spread(1U, cs::Hertz{cs::max_spread.value + 1.0}));
    static cs::PathsimDriver driver;
    VectorSource<cs::Pcm16> source{input, whole_chunks};
    VectorSink sink;
    checks.expect(!driver.run(invalid, source, sink) && sink.samples().empty(), "an invalid channel fails before any output");
}

void test_lane_chain(Checks& checks)
{
    const std::vector<double> wide = noise(48000U, 0.25, 11U);
    const double gain = cs::amplitude_from_db(campaign_gain);
    const std::vector<cs::Pcm16> narrow_in = oracle_resample(wide, wide_hz, narrow_hz, gain);
    const cs::PathsimJob job = two_path_job(31U);
    const std::vector<cs::Pcm16> narrow_out = run_driver(job, narrow_in, whole_chunks);
    std::vector<double> narrow_unit(narrow_out.size());
    std::ranges::transform(narrow_out, narrow_unit.begin(), cs::pcm16_to_unit);
    const std::vector<cs::Pcm16> expected = oracle_resample(narrow_unit, narrow_hz, wide_hz, 1.0);
    static cs::LaneChain chain;
    VectorSource<double> source{wide, odd_chunks};
    VectorSink sink;
    const m110::Result<cs::LaneChainReport> report = chain.run(job, cs::LaneChainConfig{}, source, sink);
    checks.expect(report.has_value(), "the lane chain runs");
    checks.expect(sink.samples() == expected, "the lane chain equals resampler -> pathsim -> resampler");
    checks.expect(source.rewinds() == 1U, "the chain reads its source twice");
    checks.expect(report.has_value() && report.value().input_samples == wide.size() && report.value().output_samples == expected.size(), "the chain reports its sample counts");
    // A float32 input with +inf where an interior output's window has a zero-weight tap.
    std::vector<double> with_inf = wide;
    std::span{with_inf}[inf_position] = std::numeric_limits<double>::infinity();
    VectorSource<double> bad_source{with_inf, odd_chunks};
    VectorSink bad_sink;
    checks.expect(!chain.run(job, cs::LaneChainConfig{}, bad_source, bad_sink).has_value() && bad_sink.samples().empty(), "a non-finite input sample fails the chain before any output");
}
} // namespace

int main()
{
    Checks checks;
    test_pcm(checks);
    test_resampler(checks);
    test_resampler_streams(checks);
    test_channel_spec(checks);
    test_draw_order(checks);
    test_driver(checks);
    test_lane_chain(checks);

    if (checks.failures() != 0)
    {
        std::fputs("channel-sim tests: FAIL\n", stdout);
        return 1;
    }

    std::fputs("channel-sim tests: PASS\n", stdout);
    return 0;
}
