// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// Leaf tests for scenario-wide AWGN bandwidth selection.

#include "signal_lab/awgn.hpp"

#include "signal_lab/convert.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace
{

namespace sl = signal_lab;
namespace units = signal_lab::units;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

constexpr double rate_hz = static_cast<double>(sl::stream_rate.hertz());
constexpr double passband_flatness_db = 0.02;
constexpr double two_pi = 6.283185307179586476925286766559;
constexpr std::array<sl::NoiseBandwidth, 4U> bandwidths{sl::NoiseBandwidth::khz3, sl::NoiseBandwidth::khz6, sl::NoiseBandwidth::khz9, sl::NoiseBandwidth::khz12};
constexpr std::array<std::array<std::uint32_t, 5U>, 4U> tap_bits{{
    {{0x354878b5U, 0xb6dab9bdU, 0xb985ab88U, 0x3e07dea0U, 0x3e0cd3b6U}},
    {{0x37d5c826U, 0x370ede45U, 0xba88bf3bU, 0x3e6d40e4U, 0x3e8667e9U}},
    {{0x380ceeffU, 0x37dc2bbbU, 0xbab6d848U, 0x3e96c51dU, 0x3ec664adU}},
    {{0x377cf9ecU, 0x3818368bU, 0xba90b9bcU, 0x3e9f7b53U, 0x3f033203U}},
}};
constexpr std::array<double, 4U> lower_edge_response{0.99898514947388362, 0.99886942630154252, 0.99881516936285020, 0.99881106370554884};
constexpr std::array<double, 4U> upper_edge_response{0.99933859953685944, 0.99925521574426490, 0.99913220205720754, 0.99912319770382818};
constexpr std::array<std::uint64_t, 4U> sigma_bits{0x3fcc9f25c5bfedd9ULL, 0x3fc43d136248490eULL, 0x3fc08654a2d4f6daULL, 0x3fbc9f25c5bfedd9ULL};

[[nodiscard]] double response_magnitude(const sl::NoiseBandKernel& kernel, double frequency_hz)
{
    double real = sl::as_double(kernel.half[sl::noise_band_centre]);

    for (std::uint32_t index = 0U; index < sl::noise_band_centre; ++index)
    {
        const double offset = static_cast<double>(sl::noise_band_centre - index);
        real += 2.0 * sl::as_double(kernel.half[index]) * std::cos(two_pi * frequency_hz * offset / rate_hz);
    }

    return std::abs(real);
}

[[nodiscard]] std::vector<float> render(sl::NoiseBandwidth bandwidth, std::size_t block_size)
{
    constexpr std::size_t frames = 12000U;
    std::vector<float> scratch(sl::engine_capacity_frames);
    sl::StageContext context{};
    context.reference_rms = 0.25;
    context.total_frames = frames;
    context.seed = 0x1337U;
    context.index = 2U;
    context.scratch = scratch.data();
    context.noise_bandwidth = bandwidth;
    sl::AwgnParams params{};
    params.snr = units::Ratio{12.0, units::dB};
    sl::AwgnStage stage{params};
    std::vector<float> output(frames, 0.0F);

    if (!stage.prepare(context).is_ok())
    {
        return {};
    }

    for (std::size_t first = 0U; first < output.size(); first += block_size)
    {
        const std::size_t count = std::min(block_size, output.size() - first);
        const auto result = stage.process(output.data() + first, count, first, sl::engine_capacity_frames);

        if (!result.has_value() || result.value() != count)
        {
            return {};
        }
    }

    return output;
}

[[nodiscard]] bool same_bits(std::span<const float> first, std::span<const float> second)
{
    return std::ranges::equal(first, second, [](float a, float b) { return std::bit_cast<std::uint32_t>(a) == std::bit_cast<std::uint32_t>(b); });
}

TEST_CASE("AWGN bandwidth: the enum maps only the four specified widths", "[noise_bandwidth]")
{
    CHECK(sl::noise_band_design_method == "kaiser-sinc");
    CHECK(sl::noise_band_taps == 583U);
    constexpr std::array<double, 4U> expected{3000.0, 6000.0, 9000.0, 12000.0};

    for (std::size_t index = 0U; index < bandwidths.size(); ++index)
    {
        CHECK(sl::valid_noise_bandwidth(bandwidths[index]));
        CHECK(sl::noise_band_width(bandwidths[index]).in(units::Hz) == expected[index]);
    }

    constexpr auto invalid = static_cast<sl::NoiseBandwidth>(4500U);
    CHECK_FALSE(sl::valid_noise_bandwidth(invalid));
    CHECK(sl::noise_band_width(invalid).in(units::Hz) == 0.0);
    const auto rejected_kernel = sl::noise_band_kernel(invalid);
    CHECK_FALSE(rejected_kernel.has_value());
    CHECK(rejected_kernel.error().code == m110::StatusCode::invalid_configuration);
    const auto rejected_sigma = sl::awgn_noise_sigma(units::Amplitude{0.25}, units::Ratio{10.0, units::dB}, invalid);
    CHECK_FALSE(rejected_sigma.has_value());
    CHECK(rejected_sigma.error().code == m110::StatusCode::invalid_configuration);
}

TEST_CASE("AWGN bandwidth: four shared kernels cover their passbands and scale N0 reference width", "[noise_bandwidth]")
{
    const auto& legacy = sl::noise_band_kernel();
    const auto selected_3k = sl::noise_band_kernel(sl::NoiseBandwidth::khz3);
    REQUIRE(selected_3k.has_value());
    CHECK(&selected_3k.value().get() == &legacy);
    const units::Amplitude reference{0.25};
    const units::Ratio snr{10.0, units::dB};
    const double sigma_3k = sl::awgn_noise_sigma(reference, snr).full_scale();
    constexpr std::array<double, 4U> sigma_golden{0.22360679774997900, 0.15811388300841900, 0.12909944487358100, 0.11180339887498950};
    const auto typed_sigma_3k = sl::awgn_noise_sigma(reference, snr, sl::NoiseBandwidth::khz3);
    REQUIRE(typed_sigma_3k.has_value());
    CHECK(std::bit_cast<std::uint64_t>(typed_sigma_3k.value().full_scale()) == std::bit_cast<std::uint64_t>(sigma_3k));
    constexpr std::array<std::uint32_t, 5U> tap_indices{0U, 1U, 145U, 290U, sl::noise_band_centre};

    for (std::size_t bandwidth_index = 0U; bandwidth_index < bandwidths.size(); ++bandwidth_index)
    {
        const sl::NoiseBandwidth bandwidth = bandwidths[bandwidth_index];
        const auto selected = sl::noise_band_kernel(bandwidth);
        REQUIRE(selected.has_value());
        const sl::NoiseBandKernel& kernel = selected.value().get();
        const double width = sl::noise_band_width(bandwidth).in(units::Hz);
        const double upper = 300.0 + width;
        const double centre = 300.0 + width / 2.0;

        for (std::size_t tap_index = 0U; tap_index < tap_indices.size(); ++tap_index)
        {
            CHECK(std::bit_cast<std::uint32_t>(kernel.half[tap_indices[tap_index]]) == tap_bits[bandwidth_index][tap_index]);
        }

        CHECK_THAT(response_magnitude(kernel, centre), WithinAbs(1.0, 1.0e-6));
        const double lower_magnitude = response_magnitude(kernel, 300.0);
        const double upper_magnitude = response_magnitude(kernel, upper);
        CHECK_THAT(lower_magnitude, WithinAbs(lower_edge_response[bandwidth_index], 2.0e-8));
        CHECK_THAT(upper_magnitude, WithinAbs(upper_edge_response[bandwidth_index], 2.0e-8));
        CHECK(std::abs(20.0 * std::log10(lower_magnitude)) <= passband_flatness_db);
        CHECK(std::abs(20.0 * std::log10(upper_magnitude)) <= passband_flatness_db);
        CHECK(kernel.enbw.in(units::Hz) > width);
        const auto sigma = sl::awgn_noise_sigma(reference, snr, bandwidth);
        REQUIRE(sigma.has_value());
        CHECK(std::bit_cast<std::uint64_t>(sigma.value().full_scale()) == sigma_bits[bandwidth_index]);
        CHECK_THAT(sigma.value().full_scale(), WithinAbs(sigma_golden[bandwidth_index], 1.0e-14));
        const double expected_ratio = std::sqrt(3000.0 / width);
        CHECK_THAT(sigma.value().full_scale() / sigma_3k, WithinRel(expected_ratio, 1.0e-14));
    }
}

TEST_CASE("AWGN bandwidth: kernel bank stays stable and every output is partition independent", "[noise_bandwidth]")
{
    std::array<const sl::NoiseBandKernel*, bandwidths.size()> first_addresses{};

    for (std::size_t index = 0U; index < bandwidths.size(); ++index)
    {
        const auto kernel = sl::noise_band_kernel(bandwidths[index]);
        REQUIRE(kernel.has_value());
        first_addresses[index] = &kernel.value().get();
        CHECK(kernel.value().get().half[sl::noise_band_centre] > 0.0F);
        const std::vector<float> whole = render(bandwidths[index], sl::engine_block_frames);
        const std::vector<float> split = render(bandwidths[index], 257U);
        REQUIRE(whole.size() == 12000U);
        REQUIRE(split.size() == whole.size());
        CHECK(same_bits(whole, split));
    }

    for (std::size_t index = 0U; index < bandwidths.size(); ++index)
    {
        const auto again = sl::noise_band_kernel(bandwidths[index]);
        REQUIRE(again.has_value());
        CHECK(first_addresses[index] == &again.value().get());
    }
}

} // namespace
