// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

#include "signal_lab/awgn.hpp"
#include "signal_lab/det_math.hpp"
#include "signal_lab/impairment.hpp"
#include "signal_lab/mixer.hpp"
#include "signal_lab/stream_identity.hpp"
#include "signal_lab/watterson.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

namespace sl = signal_lab;

TEST_CASE("Stream identity pins SplitMix64 key vectors and zero-ID compatibility", "[rng][stream-identity]")
{
    CHECK(sl::stream_identity_algorithm_version == 1U);
    constexpr sl::StageRandomKey legacy = sl::resolve_stage_random_key(0x0123456789ABCDEFULL, 7U, 0U);
    static_assert(legacy.seed == 0x0123456789ABCDEFULL);
    static_assert(legacy.index == 7U);
    sl::det::Pcg32 direct_legacy{0x0123456789ABCDEFULL, 7U, 7U};
    sl::det::Pcg32 resolved_legacy{legacy.seed, 7U, legacy.index};
    CHECK(direct_legacy.next() == resolved_legacy.next());
    CHECK(direct_legacy.next() == resolved_legacy.next());
    constexpr sl::StageRandomKey zero_seed = sl::resolve_stage_random_key(0U, 0U, 1U);
    static_assert(zero_seed.seed == 0x5A93E3278FED8726ULL);
    static_assert(zero_seed.index == 0U);
    constexpr sl::StageRandomKey known = sl::resolve_stage_random_key(0x0123456789ABCDEFULL, 7U, 42U);
    static_assert(known.seed == 0x005F869427719E8FULL);
    static_assert(known.index == 0U);
    constexpr sl::StageRandomKey maximum = sl::resolve_stage_random_key(UINT64_MAX, UINT32_MAX, UINT32_MAX);
    static_assert(maximum.seed == 0xC736A20832A4BD0AULL);
    static_assert(maximum.index == 0U);
    CHECK(true);
}

namespace
{

constexpr std::uint64_t master_seed = 0xA5A55A5A01234567ULL;
constexpr std::uint32_t explicit_id = 0x10203040U;
constexpr std::size_t test_frames = 8192U;

[[nodiscard]] sl::StageContext context(std::uint32_t ordinal, std::uint32_t stream_id, float* scratch = nullptr) noexcept
{
    return sl::StageContext{
        .reference_rms = 0.25, .total_frames = test_frames, .seed = master_seed, .index = ordinal, .scratch = scratch, .noise_bandwidth = sl::NoiseBandwidth::khz3, .stream_id = stream_id};
}

[[nodiscard]] std::vector<float> source()
{
    std::vector<float> samples(test_frames);

    for (std::size_t frame = 0U; frame < samples.size(); ++frame)
    {
        samples[frame] = static_cast<float>(0.2 * sl::det::sin(sl::det::two_pi * 1000.0 * static_cast<double>(frame) / 48000.0));
    }

    return samples;
}

template <typename Stage> [[nodiscard]] std::vector<float> process_stage(Stage& stage, const sl::StageContext& stage_context)
{
    std::vector<float> output = source();
    REQUIRE(stage.prepare(stage_context).is_ok());

    for (std::size_t offset = 0U; offset < output.size(); offset += 127U)
    {
        const std::size_t frames = std::min<std::size_t>(127U, output.size() - offset);
        REQUIRE(stage.process(output.data() + offset, frames, offset, frames).has_value());
    }

    return output;
}

[[nodiscard]] std::vector<float> run_awgn(std::uint32_t ordinal, std::uint32_t stream_id)
{
    std::array<float, sl::engine_capacity_frames> scratch{};
    sl::AwgnParams params{};
    params.snr = sl::units::Ratio{0.0, sl::units::dB};
    sl::AwgnStage stage{params};
    return process_stage(stage, context(ordinal, stream_id, scratch.data()));
}

[[nodiscard]] std::vector<float> run_cw(std::uint32_t ordinal, std::uint32_t stream_id)
{
    sl::CwParams params{};
    params.frequency_hz = 6000.0;
    params.ci_db = 0.0;
    params.random_phase = true;
    params.ramp_seconds = 0.0;
    params.fsk.enabled = true;
    params.fsk.baud = 12000.0;
    params.fsk.shift = sl::units::FrequencyDelta{2000.0, sl::units::Hz};
    params.fsk.pattern = sl::FskPattern::random;
    params.markov.enabled = true;
    params.markov.mean_on_seconds = 2.0 / 48000.0;
    params.markov.mean_off_seconds = 2.0 / 48000.0;
    params.fading.enabled = true;
    params.fading.spread = sl::units::FrequencyDelta{0.5, sl::units::Hz};
    alignas(sl::impairment_alignment) std::array<std::byte, 4096U> storage{};
    const sl::PlacedStage placed = sl::construct_cw(params, storage.data(), storage.size());
    REQUIRE(placed.has_value());
    return process_stage(placed.value().get(), context(ordinal, stream_id));
}

[[nodiscard]] std::vector<float> run_impulse(std::uint32_t ordinal, std::uint32_t stream_id, bool ring_noise)
{
    sl::ImpulseParams params{};
    params.peak_db = 0.0;
    params.decay_ms = 0.25;
    params.truncate_tau = 2.0;
    params.random_phase = true;
    params.ring_noise = ring_noise;
    params.poisson = true;
    params.rate_per_sec = 1000.0;
    alignas(sl::impairment_alignment) std::array<std::byte, sl::impairment_arena_bytes> storage{};
    const sl::PlacedStage placed = sl::construct_impulse(params, storage.data(), storage.size());
    REQUIRE(placed.has_value());
    return process_stage(placed.value().get(), context(ordinal, stream_id));
}

[[nodiscard]] std::vector<float> run_fade(std::uint32_t ordinal, std::uint32_t stream_id)
{
    sl::FadeParams params{};
    params.depth_db = 20.0;
    params.duration_ms = 0.25;
    params.schedule = sl::FadeSchedule::poisson;
    params.rate_per_sec = 1000.0;
    alignas(sl::impairment_alignment) std::array<std::byte, sl::impairment_arena_bytes> storage{};
    const sl::PlacedStage placed = sl::construct_fade(params, storage.data(), storage.size());
    REQUIRE(placed.has_value());
    return process_stage(placed.value().get(), context(ordinal, stream_id));
}

[[nodiscard]] std::vector<float> run_watterson(std::uint32_t ordinal, std::uint32_t stream_id)
{
    sl::WattersonParams params{};
    params.path_count = 1U;
    params.paths[0].fading = sl::PathFading::rayleigh;
    params.paths[0].spread = sl::units::FrequencyDelta{0.5, sl::units::Hz};
    sl::WattersonChannel channel{};
    sl::WattersonStage stage{params, &channel, nullptr};
    return process_stage(stage, context(ordinal, stream_id));
}

} // namespace

TEST_CASE("Explicit stream IDs stabilize real stochastic stages across ordinal changes", "[rng][stream-identity]")
{
    const auto input = source();
    const auto awgn = run_awgn(2U, explicit_id);
    CHECK(awgn == run_awgn(19U, explicit_id));
    CHECK(awgn != input);
    CHECK(awgn != run_awgn(2U, explicit_id + 1U));
    const auto cw = run_cw(2U, explicit_id);
    CHECK(cw == run_cw(19U, explicit_id));
    CHECK(cw != run_cw(2U, explicit_id + 1U));
    const auto impulse_phase = run_impulse(2U, explicit_id, false);
    CHECK(impulse_phase == run_impulse(19U, explicit_id, false));
    CHECK(impulse_phase != input);
    CHECK(impulse_phase != run_impulse(2U, explicit_id + 1U, false));
    const auto impulse_noise = run_impulse(2U, explicit_id, true);
    CHECK(impulse_noise == run_impulse(19U, explicit_id, true));
    CHECK(impulse_noise != input);
    CHECK(impulse_noise != run_impulse(2U, explicit_id + 1U, true));
    const auto fade = run_fade(2U, explicit_id);
    CHECK(fade == run_fade(19U, explicit_id));
    CHECK(fade != input);
    CHECK(fade != run_fade(2U, explicit_id + 1U));
    const auto watterson = run_watterson(2U, explicit_id);
    CHECK(watterson == run_watterson(19U, explicit_id));
    CHECK(watterson != input);
    CHECK(watterson != run_watterson(2U, explicit_id + 1U));
}

TEST_CASE("Omitted stream IDs retain ordinal-dependent stage output", "[rng][stream-identity]")
{
    CHECK(run_awgn(2U, 0U) != run_awgn(19U, 0U));
}

TEST_CASE("StageContext keeps the ordinal and resolves its explicit stream independently", "[rng][stream-identity]")
{
    const sl::StageContext context{
        .reference_rms = 0.25, .total_frames = 100U, .seed = 0x0123456789ABCDEFULL, .index = 7U, .scratch = nullptr, .noise_bandwidth = sl::NoiseBandwidth::khz3, .stream_id = 42U};
    const sl::StageRandomKey key = context.random_key();
    CHECK(context.index == 7U);
    CHECK(key.seed == 0x005F869427719E8FULL);
    CHECK(key.index == 0U);
}
