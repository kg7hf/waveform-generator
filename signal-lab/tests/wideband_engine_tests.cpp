// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// Host integration checks for the scenario-wide AWGN reference bandwidth. These
// complement the kernel's unit tests: parsing, engine propagation, partition
// invariance, N0 x BW scaling, and the wider passband's E.7 reference response.

#include "allocation_counter.hpp"
#include "signal_lab/awgn.hpp"
#include "signal_lab/convert.hpp"
#include "signal_lab/det_math.hpp"
#include "signal_lab/mixer.hpp"
#include "signal_lab/noise_bandwidth.hpp"
#include "signal_lab/scenario.hpp"
#include "signal_lab/units/sampling.hpp"
#include "signal_lab/watterson.hpp"
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
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{

namespace sl = signal_lab;
using Catch::Matchers::WithinAbs;

constexpr double sample_rate_hz = 48000.0;
constexpr double pi = 3.141592653589793238462643383279502884;
constexpr double reference_rms = 0.125;
constexpr double snr_db = 12.0;
constexpr std::size_t engine_block = sl::engine_block_frames;
constexpr std::array wide_bandwidths{sl::NoiseBandwidth::khz6, sl::NoiseBandwidth::khz9, sl::NoiseBandwidth::khz12};
constexpr std::array all_bandwidths{sl::NoiseBandwidth::khz3, sl::NoiseBandwidth::khz6, sl::NoiseBandwidth::khz9, sl::NoiseBandwidth::khz12};
constexpr std::uint64_t noise_frames = std::uint64_t{32U} * 48000U;
constexpr std::size_t welch_segment = 4096U;
constexpr double psd_tolerance_db = 0.10;
constexpr double subband_flatness_tolerance_db = 0.20;
constexpr double subband_width_hz = 1000.0;
constexpr double first_interior_hz = 500.0;
constexpr std::size_t max_psd_subbands = 12U;
constexpr double passband_tolerance_db = 0.50;
constexpr std::uint64_t source_seed = 0x7123U;

[[nodiscard]] constexpr std::uint32_t bandwidth_hz(sl::NoiseBandwidth bandwidth)
{
    return static_cast<std::uint32_t>(std::to_underlying(bandwidth));
}

[[nodiscard]] std::string scenario_json(std::uint32_t bandwidth, bool with_watterson, double snr = snr_db)
{
    std::string text = "{\"noise_bandwidth_hz\":" + std::to_string(bandwidth) + ",\"seed\":41,\"source_gain_db\":0,\"impairments\":[";

    if (with_watterson)
    {
        text += R"({"type":"watterson","paths":[{"fading":"fixed"},{"delay_ms":2,"fading":"fixed","phase_deg":90}]},)";
    }

    text += R"({"type":"awgn","snr_db":)" + std::to_string(snr) + "}]}";
    return text;
}

[[nodiscard]] std::string legacy_default_json()
{
    return R"({"seed":41,"source_gain_db":0,"impairments":[{"type":"awgn","snr_db":12}]})";
}

[[nodiscard]] std::string explicit_default_json()
{
    return R"({"noise_bandwidth_hz":3000,"seed":41,"source_gain_db":0,"impairments":[{"type":"awgn","snr_db":12}]})";
}

[[nodiscard]] sl::WattersonChannel& channel()
{
    constinit static sl::WattersonChannel value{};
    return value;
}

[[nodiscard]] std::vector<float> source(std::size_t frames)
{
    sl::det::Pcg32 generator{source_seed, 0U, 0U};
    std::vector<float> result(frames);

    for (float& sample : result)
    {
        sample = sl::to_float((generator.uniform() - 0.5) * 0.4);
    }

    return result;
}

[[nodiscard]] double rms(std::span<const float> samples)
{
    double energy = 0.0;

    for (const float sample : samples)
    {
        const double value = sl::as_double(sample);
        energy += value * value;
    }

    return std::sqrt(energy / static_cast<double>(samples.size()));
}

struct Rendered
{
    std::vector<float> samples;
    bool ok{};
};

[[nodiscard]] Rendered render(sl::Engine& engine, std::span<const float> input, std::size_t partition)
{
    Rendered result{};
    std::vector<float> output(sl::engine_capacity_frames);
    result.samples.reserve(input.size());
    result.ok = true;

    for (std::size_t offset = 0U; offset < input.size(); offset += partition)
    {
        const std::size_t count = std::min(partition, input.size() - offset);
        const m110::Result<std::size_t> produced = engine.process(input.subspan(offset, count), output);

        if (!produced.has_value() || produced.value() != count)
        {
            result.ok = false;
            return result;
        }

        result.samples.insert(result.samples.end(), output.begin(), output.begin() + static_cast<std::ptrdiff_t>(count));
    }

    return result;
}

[[nodiscard]] double kernel_power(const sl::NoiseBandKernel& kernel, double frequency_hz)
{
    const std::span<const float> half{kernel.half};
    const std::uint32_t centre = sl::noise_band_centre;
    double response = sl::as_double(half[centre]);

    for (std::uint32_t tap = 0U; tap < centre; ++tap)
    {
        const double offset = sl::as_double(centre - tap);
        response += 2.0 * sl::as_double(half[tap]) * std::cos(2.0 * pi * frequency_hz * offset / sample_rate_hz);
    }

    return response * response;
}

[[nodiscard]] double psd_db(double ratio)
{
    return 10.0 * std::log10(ratio);
}

} // namespace

TEST_CASE("wideband engine: absent means 3 kHz; exact supported Hz values parse; malformed values refuse", "[awgn][bandwidth][scenario]")
{
    auto scenario = std::make_unique<sl::Scenario>();
    const std::string legacy = legacy_default_json();
    REQUIRE(sl::parse_scenario(legacy.data(), legacy.size(), *scenario).is_ok());
    CHECK(scenario->noise_bandwidth == sl::NoiseBandwidth::khz3);

    for (const sl::NoiseBandwidth bandwidth : all_bandwidths)
    {
        const std::string text = scenario_json(bandwidth_hz(bandwidth), false);
        INFO(text);
        REQUIRE(sl::parse_scenario(text.data(), text.size(), *scenario).is_ok());
        CHECK(scenario->noise_bandwidth == bandwidth);
    }

    constexpr std::array invalid_fields{
        std::string_view{"\"3000\""}, std::string_view{"true"}, std::string_view{"null"},  std::string_view{"0"},     std::string_view{"2999"},
        std::string_view{"3000.5"},   std::string_view{"4500"}, std::string_view{"12001"}, std::string_view{"24000"},
    };

    for (const std::string_view field : invalid_fields)
    {
        const std::string text = "{\"noise_bandwidth_hz\":" + std::string{field} + ",\"impairments\":[]}";
        INFO(text);
        CHECK_FALSE(sl::parse_scenario(text.data(), text.size(), *scenario).is_ok());
    }

    constexpr std::string_view duplicate = R"({"noise_bandwidth_hz":3000,"noise_bandwidth_hz":6000,"impairments":[]})";
    CHECK_FALSE(sl::parse_scenario(duplicate.data(), duplicate.size(), *scenario).is_ok());
    scenario->noise_bandwidth = static_cast<sl::NoiseBandwidth>(4500U);
    CHECK_FALSE(sl::validate_scenario(*scenario).is_ok());
}

TEST_CASE("wideband engine: Watterson plus AWGN output is unchanged by block partition for each width", "[awgn][bandwidth][engine]")
{
    constexpr std::size_t frames = 48000U;
    constexpr std::size_t odd_partition = 1001U;
    const std::vector<float> input = source(frames);
    const double input_rms = rms(input);
    auto scenario = std::make_unique<sl::Scenario>();
    auto engine = std::make_unique<sl::Engine>();

    for (const sl::NoiseBandwidth bandwidth : all_bandwidths)
    {
        const std::string text = scenario_json(bandwidth_hz(bandwidth), true);
        REQUIRE(sl::parse_scenario(text.data(), text.size(), *scenario).is_ok());
        REQUIRE(engine->configure(*scenario, input_rms, frames, sl::StageResources{&channel(), nullptr}).is_ok());
        const Rendered regular = render(*engine, input, engine_block);
        REQUIRE(regular.ok);
        REQUIRE(sl::parse_scenario(text.data(), text.size(), *scenario).is_ok());
        REQUIRE(engine->configure(*scenario, input_rms, frames, sl::StageResources{&channel(), nullptr}).is_ok());
        const Rendered odd = render(*engine, input, odd_partition);
        REQUIRE(odd.ok);
        INFO("bandwidth " << bandwidth_hz(bandwidth) << " Hz");
        REQUIRE(regular.samples.size() == odd.samples.size());
        CHECK(std::ranges::equal(regular.samples, odd.samples, [](float lhs, float rhs) { return std::bit_cast<std::uint32_t>(lhs) == std::bit_cast<std::uint32_t>(rhs); }));
    }
}

TEST_CASE("wideband engine: wider kernels meet the E.7 reference response at centre and quarter-band offsets", "[awgn][bandwidth][kernel]")
{
    for (const sl::NoiseBandwidth bandwidth : wide_bandwidths)
    {
        const auto selected = sl::noise_band_kernel(bandwidth);
        REQUIRE(selected.has_value());
        const sl::NoiseBandKernel& kernel = selected.value().get();
        const double width = static_cast<double>(bandwidth_hz(bandwidth));
        const double centre_hz = 300.0 + width / 2.0;
        const std::array frequencies{centre_hz - width / 4.0, centre_hz, centre_hz + width / 4.0};

        for (const double frequency : frequencies)
        {
            INFO("bandwidth " << width << " Hz at " << frequency << " Hz");
            const double power_db = psd_db(kernel_power(kernel, frequency));
            CHECK_THAT(power_db, WithinAbs(0.0, passband_tolerance_db));
        }
    }
}

TEST_CASE("wideband engine: in-band PSD times declared bandwidth follows the reference-power SNR contract", "[awgn][bandwidth][statistics]")
{
    constexpr double commanded_snr_db = 12.0;
    constexpr double decade = 10.0;
    constexpr double window_energy_per_sample = 0.375; // periodic Hann used by average_periodogram
    const std::vector<float> silence(engine_block, 0.0F);
    std::vector<float> output(sl::engine_capacity_frames);
    std::vector<signal_lab_test::Complex> samples;
    samples.reserve(noise_frames);
    auto scenario = std::make_unique<sl::Scenario>();
    auto engine = std::make_unique<sl::Engine>();

    for (const sl::NoiseBandwidth bandwidth : all_bandwidths)
    {
        const std::string text = scenario_json(bandwidth_hz(bandwidth), false, commanded_snr_db);
        REQUIRE(sl::parse_scenario(text.data(), text.size(), *scenario).is_ok());
        REQUIRE(engine->configure(*scenario, reference_rms, noise_frames).is_ok());
        samples.clear();

        for (std::uint64_t done = 0U; done < noise_frames; done += engine_block)
        {
            const std::size_t count = static_cast<std::size_t>(std::min<std::uint64_t>(engine_block, noise_frames - done));
            const m110::Result<std::size_t> produced = engine->process(std::span<const float>{silence}.first(count), output);
            REQUIRE(produced.has_value());
            REQUIRE(produced.value() == count);

            for (std::size_t index = 0U; index < count; ++index)
            {
                samples.emplace_back(sl::as_double(output[index]), 0.0);
            }
        }

        const std::vector<double> periodogram = signal_lab_test::average_periodogram(samples, welch_segment, signal_lab_test::Window::hann);
        const double bin_hz = sample_rate_hz / static_cast<double>(welch_segment);
        const double upper_hz = 300.0 + static_cast<double>(bandwidth_hz(bandwidth));
        double psd_sum = 0.0;
        std::size_t psd_bins = 0U;
        std::array<double, max_psd_subbands> subband_sums{};
        std::array<std::size_t, max_psd_subbands> subband_counts{};

        // Average the one-sided PSD only over the flat interior. Multiplying this
        // density by the nominal bandwidth tests N0 x BW directly; ENBW-corrected
        // total variance is deliberately not used as an in-band measurement.
        for (std::size_t bin = 1U; bin < welch_segment / 2U; ++bin)
        {
            const double frequency = static_cast<double>(bin) * bin_hz;

            if (frequency >= first_interior_hz && frequency <= upper_hz - first_interior_hz)
            {
                const double one_sided_psd = 2.0 * periodogram[bin] / (sample_rate_hz * static_cast<double>(welch_segment) * window_energy_per_sample);
                psd_sum += one_sided_psd;
                ++psd_bins;
                const std::size_t subband = static_cast<std::size_t>((frequency - first_interior_hz) / subband_width_hz);
                REQUIRE(subband < subband_sums.size());
                subband_sums[subband] += one_sided_psd;
                ++subband_counts[subband];
            }
        }

        REQUIRE(psd_bins > 0U);
        const double n0 = psd_sum / static_cast<double>(psd_bins);
        const double declared_power = reference_rms * reference_rms * std::pow(decade, -commanded_snr_db / 10.0);
        const double measured_db = psd_db(n0 * static_cast<double>(bandwidth_hz(bandwidth)) / declared_power);
        double lowest_subband_db = std::numeric_limits<double>::infinity();
        double highest_subband_db = -std::numeric_limits<double>::infinity();
        std::size_t accepted_subbands = 0U;

        for (std::size_t index = 0U; index < subband_sums.size(); ++index)
        {
            if (subband_counts[index] < 40U)
            {
                continue;
            }

            const double subband_n0 = subband_sums[index] / static_cast<double>(subband_counts[index]);
            const double subband_db = psd_db(subband_n0 / n0);
            lowest_subband_db = std::min(lowest_subband_db, subband_db);
            highest_subband_db = std::max(highest_subband_db, subband_db);
            ++accepted_subbands;
        }

        REQUIRE(accepted_subbands >= 2U);
        const double measured_flatness_db = highest_subband_db - lowest_subband_db;
        INFO("bandwidth " << bandwidth_hz(bandwidth) << " Hz; N0xBW error " << measured_db << " dB; flatness " << measured_flatness_db << " dB");
        CHECK_THAT(measured_db, WithinAbs(0.0, psd_tolerance_db));
        CHECK(measured_flatness_db <= subband_flatness_tolerance_db);
    }
}

TEST_CASE("wideband engine: omitted 3 kHz setting and explicit 3000 Hz are bit-identical", "[awgn][bandwidth][compatibility]")
{
    constexpr std::size_t frames = 8192U;
    const std::vector<float> input = source(frames);
    const double input_rms = rms(input);
    auto scenario = std::make_unique<sl::Scenario>();
    auto engine = std::make_unique<sl::Engine>();
    const std::string absent = legacy_default_json();
    const std::string explicit_value = explicit_default_json();
    REQUIRE(sl::parse_scenario(absent.data(), absent.size(), *scenario).is_ok());
    CHECK(scenario->noise_bandwidth == sl::NoiseBandwidth::khz3);
    REQUIRE(engine->configure(*scenario, input_rms, frames).is_ok());
    const Rendered old_default = render(*engine, input, engine_block);
    REQUIRE(old_default.ok);
    REQUIRE(sl::parse_scenario(explicit_value.data(), explicit_value.size(), *scenario).is_ok());
    REQUIRE(engine->configure(*scenario, input_rms, frames).is_ok());
    const Rendered selected_default = render(*engine, input, engine_block);
    REQUIRE(selected_default.ok);
    CHECK(std::ranges::equal(old_default.samples, selected_default.samples, [](float lhs, float rhs) { return std::bit_cast<std::uint32_t>(lhs) == std::bit_cast<std::uint32_t>(rhs); }));
}

TEST_CASE("wideband engine: configuration and processing allocate no memory", "[awgn][bandwidth][allocation]")
{
    const std::string text = scenario_json(12000U, true);
    auto scenario = std::make_unique<sl::Scenario>();
    auto engine = std::make_unique<sl::Engine>();
    REQUIRE(sl::parse_scenario(text.data(), text.size(), *scenario).is_ok());
    const std::vector<float> input = source(engine_block);
    const double input_rms = rms(input);
    std::vector<float> output(sl::engine_capacity_frames);
    bool configured = false;
    bool processed = false;
    std::uint64_t allocations = 0U;
    {
        const signal_lab_test::CountedRegion region;
        configured = engine->configure(*scenario, input_rms, engine_block, sl::StageResources{&channel(), nullptr}).is_ok();

        if (configured)
        {
            const m110::Result<std::size_t> count = engine->process(input, output);
            processed = count.has_value() && count.value() == engine_block;
        }

        allocations = region.allocations();
    }

    CHECK(configured);
    CHECK(processed);
    CHECK(allocations == 0U);
}

TEST_CASE("wideband engine: an invalid bandwidth enum is refused by the kernel selector", "[awgn][bandwidth][refusal]")
{
    const auto selected = sl::noise_band_kernel(static_cast<sl::NoiseBandwidth>(4500U));
    CHECK_FALSE(selected.has_value());
}
