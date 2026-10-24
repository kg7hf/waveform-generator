// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

#include "allocation_counter.hpp"
#include "signal_lab/markov_gate.hpp"
#include "signal_lab/second_source.hpp"
#include "signal_lab/stream_identity.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace sl = signal_lab;
namespace units = signal_lab::units;

namespace
{

[[nodiscard]] sl::MarkovGateParams markov_params(double on_frames, double off_frames) noexcept
{
    return sl::MarkovGateParams{.enabled = true, .mean_on_seconds = on_frames / static_cast<double>(sl::sample_rate_hz), .mean_off_seconds = off_frames / static_cast<double>(sl::sample_rate_hz)};
}

class TestSource final : public sl::SecondarySource
{
public:
    TestSource(std::span<const float> samples, double rms) noexcept : mSamples{samples}, mRms{rms} {}

    [[nodiscard]] std::uint64_t frames() const noexcept override { return mSamples.size(); }

    [[nodiscard]] double reference_rms() const noexcept override { return mRms; }

    [[nodiscard]] m110::Status read(std::uint64_t first, std::span<float> output) noexcept override
    {
        ++mReads;

        if (mFail)
        {
            return m110::Status{m110::StatusCode::io_error, "test source read failed"};
        }

        if (first > mSamples.size() || output.size() > mSamples.size() - first)
        {
            return m110::Status{m110::StatusCode::invalid_argument, "test source read out of bounds"};
        }

        if (!output.empty())
        {
            mLastFirst = first;
            mLastCount = output.size();
        }

        std::ranges::copy(mSamples.subspan(static_cast<std::size_t>(first), output.size()), output.begin());
        return m110::Status::success();
    }

    void fail(bool enabled) noexcept { mFail = enabled; }

    [[nodiscard]] std::uint32_t reads() const noexcept { return mReads; }

    [[nodiscard]] std::uint64_t last_first() const noexcept { return mLastFirst; }

    [[nodiscard]] std::size_t last_count() const noexcept { return mLastCount; }

private:
    std::span<const float> mSamples{};
    double mRms{};
    std::uint64_t mLastFirst{};
    std::size_t mLastCount{};
    std::uint32_t mReads{};
    bool mFail{};
};

[[nodiscard]] sl::StageContext context(float* scratch, double desired_rms = 1.0, std::uint32_t stream_id = 0U, std::uint32_t index = 3U, std::uint64_t seed = 9U) noexcept
{
    return sl::StageContext{.reference_rms = desired_rms, .total_frames = 0U, .seed = seed, .index = index, .scratch = scratch, .stream_id = stream_id};
}

[[nodiscard]] std::vector<float> render(sl::SecondSourceStage& stage, std::size_t block_size, std::size_t total_frames, double desired_rms, sl::SecondarySource& source, std::uint32_t stream_id = 0U,
                                        std::uint32_t index = 3U, std::uint64_t seed = 9U)
{
    std::array<float, sl::engine_capacity_frames> scratch{};
    REQUIRE(stage.prepare(context(scratch.data(), desired_rms, stream_id, index, seed)).is_ok());
    std::vector<float> output(total_frames, 0.0F);
    std::size_t offset = 0U;

    while (offset < output.size())
    {
        const std::size_t count = std::min(block_size, output.size() - offset);
        const auto result = stage.process(output.data() + offset, count, offset, count);
        REQUIRE(result.has_value());
        REQUIRE(result.value() == count);
        offset += count;
    }

    (void)source;
    return output;
}

[[nodiscard]] std::vector<float> shifted_reference(std::span<const float> input, std::size_t block_size, units::FrequencyDelta offset)
{
    sl::FrequencyShiftParams params{};
    params.offset = offset;
    sl::FrequencyShiftStage stage{params};
    REQUIRE(stage.prepare(sl::StageContext{}).is_ok());
    std::vector<float> output(input.begin(), input.end());
    std::size_t cursor = 0U;

    while (cursor < output.size())
    {
        const std::size_t count = std::min(block_size, output.size() - cursor);
        const auto result = stage.process(output.data() + cursor, count, cursor, count);
        REQUIRE(result.has_value());
        cursor += count;
    }

    return output;
}

} // namespace

TEST_CASE("second source overlays only its selected excerpt at the requested S/I", "[second-source]")
{
    constexpr std::array<float, 5U> samples{0.1F, 0.25F, -0.5F, 0.75F, 0.2F};
    sl::FixedSecondarySource fixed{samples, 0.5};
    sl::SecondSourceParams params{};
    params.si = units::Ratio::from_db(6.0);
    params.start_frame = 2U;
    params.source_start_frame = 1U;
    params.duration_frames = 3U;
    sl::SecondSourceStage stage{params, &fixed};
    std::array<float, sl::engine_capacity_frames> scratch{};
    REQUIRE(stage.prepare(context(scratch.data(), 2.0)).is_ok());
    std::array<float, 7U> block{};
    const auto result = stage.process(block.data(), block.size(), 0U, block.size());
    REQUIRE(result.has_value());
    const double scale = 2.0 / 0.5 * std::pow(10.0, -6.0 / 20.0);
    CHECK(block[0] == 0.0F);
    CHECK(block[1] == 0.0F);
    CHECK(block[2] == Catch::Approx(static_cast<float>(samples[1] * scale)).margin(1.0e-7F));
    CHECK(block[3] == Catch::Approx(static_cast<float>(samples[2] * scale)).margin(1.0e-7F));
    CHECK(block[4] == Catch::Approx(static_cast<float>(samples[3] * scale)).margin(1.0e-7F));
    CHECK(block[5] == 0.0F);
    CHECK(fixed.frames() == samples.size());
    CHECK(stage.stats().events_scheduled == 1U);
    CHECK(stage.stats().events_applied == 1U);
    CHECK(stage.stats().component_frames == 3U);
}

TEST_CASE("second source implicit duration consumes the remaining source and partitions identically", "[second-source]")
{
    std::array<float, 512U> samples{};

    for (std::size_t index = 0U; index < samples.size(); ++index)
    {
        samples[index] = static_cast<float>(0.2 * std::sin(2.0 * 3.141592653589793 * 1200.0 * static_cast<double>(index) / 48000.0));
    }

    TestSource first_source{samples, 0.25};
    TestSource second_source{samples, 0.25};
    constexpr std::size_t total = 544U;
    sl::SecondSourceParams params{};
    params.start_frame = 37U;
    params.source_start_frame = 21U;
    sl::SecondSourceStage one_block{params, &first_source};
    sl::SecondSourceStage partitioned{params, &second_source};
    const auto one = render(one_block, total, total, 0.5, first_source);
    const auto many = render(partitioned, 37U, total, 0.5, second_source);
    CHECK(one == many);
    CHECK(first_source.last_first() == 21U);
    CHECK(first_source.last_count() == samples.size() - 21U);
    CHECK(one_block.stats().component_frames == samples.size() - 21U);
    CHECK(std::ranges::all_of(one.begin() + 528, one.end(), [](float sample) { return sample == 0.0F; }));
}

TEST_CASE("second source offset is window-relative and does not leak beyond its excerpt", "[second-source]")
{
    constexpr std::size_t total = 768U;
    std::array<float, 760U> samples{};

    for (std::size_t index = 0U; index < samples.size(); ++index)
    {
        samples[index] = static_cast<float>(0.1 * std::sin(2.0 * 3.141592653589793 * 1800.0 * static_cast<double>(index) / 48000.0));
    }

    constexpr std::uint64_t target_start = 37U;
    constexpr std::uint64_t source_start = 29U;

    for (const double offset_hz : std::array<double, 2U>{37.0, -37.0})
    {
        for (const std::uint64_t duration : std::array<std::uint64_t, 2U>{total - target_start, 300U})
        {
            TestSource source{samples, 1.0};
            sl::SecondSourceParams params{};
            params.si = units::Ratio::from_db(0.0);
            params.start_frame = target_start;
            params.source_start_frame = source_start;
            params.duration_frames = duration;
            params.frequency_offset = units::FrequencyDelta{offset_hz, units::Hz};
            sl::SecondSourceStage stage{params, &source};
            const auto actual = render(stage, 127U, total, 1.0, source);
            const auto excerpt = std::span{samples}.subspan(source_start, duration);
            const auto shifted_excerpt = shifted_reference(excerpt, 127U, params.frequency_offset);
            std::vector<float> expected(total, 0.0F);
            std::ranges::copy(shifted_excerpt, expected.begin() + target_start);
            CHECK(actual == expected);
            CHECK(std::ranges::all_of(actual.begin(), actual.begin() + target_start, [](float sample) { return sample == 0.0F; }));

            TestSource partition_source{samples, 1.0};
            sl::SecondSourceStage partitioned{params, &partition_source};
            CHECK(render(partitioned, 1U, total, 1.0, partition_source) == actual);
            CHECK(render(stage, 127U, total, 1.0, source) == actual);
        }
    }
}

TEST_CASE("second source rejects invalid parameters, provider geometry and gain", "[second-source]")
{
    sl::SecondSourceParams params{};
    params.si = units::Ratio::from_db(120.1);
    CHECK_FALSE(sl::validate_second_source_params(params).is_ok());
    params = sl::SecondSourceParams{};
    params.start_frame = sl::max_second_source_frames;
    params.duration_frames = 1U;
    CHECK_FALSE(sl::validate_second_source_params(params).is_ok());
    params = sl::SecondSourceParams{};
    params.frequency_offset = units::FrequencyDelta{75.01, units::Hz};
    CHECK_FALSE(sl::validate_second_source_params(params).is_ok());
    constexpr std::array<float, 4U> samples{1.0F, 0.5F, -0.5F, -1.0F};
    std::array<float, sl::engine_capacity_frames> scratch{};
    const sl::SecondSourceParams default_params{};
    TestSource silent{samples, 0.0};
    sl::SecondSourceStage no_rms{default_params, &silent};
    CHECK_FALSE(no_rms.prepare(context(scratch.data())).is_ok());
    TestSource good{samples, 1.0};
    sl::SecondSourceStage missing_scratch{default_params, &good};
    CHECK_FALSE(missing_scratch.prepare(context(nullptr)).is_ok());
    sl::SecondSourceParams outside{};
    outside.source_start_frame = samples.size();
    sl::SecondSourceStage out_of_range{outside, &good};
    CHECK_FALSE(out_of_range.prepare(context(scratch.data())).is_ok());
    sl::SecondSourceParams beyond_eof{};
    beyond_eof.duration_frames = samples.size() + 1U;
    sl::SecondSourceStage too_long{beyond_eof, &good};
    CHECK_FALSE(too_long.prepare(context(scratch.data())).is_ok());
    sl::SecondSourceParams bad_gain{};
    TestSource tiny_rms{samples, std::numeric_limits<double>::min()};
    sl::SecondSourceStage overflow{bad_gain, &tiny_rms};
    CHECK_FALSE(overflow.prepare(context(scratch.data(), std::numeric_limits<double>::max())).is_ok());
    sl::SecondSourceParams invalid_markov{};
    invalid_markov.markov = markov_params(4.0, 3.0);
    CHECK(sl::validate_second_source_params(invalid_markov).is_ok());
    invalid_markov.markov.mean_on_seconds = 0.5 / static_cast<double>(sl::sample_rate_hz);
    CHECK_FALSE(sl::validate_second_source_params(invalid_markov).is_ok());
    invalid_markov.markov = markov_params(4.0, 3.0);
    invalid_markov.markov.mean_off_seconds = std::numeric_limits<double>::infinity();
    CHECK_FALSE(sl::validate_second_source_params(invalid_markov).is_ok());
    invalid_markov.markov = markov_params(4.0, 3.0);
    invalid_markov.markov.mean_on_seconds = std::numeric_limits<double>::quiet_NaN();
    CHECK_FALSE(sl::validate_second_source_params(invalid_markov).is_ok());
    constexpr std::array<float, 1U> huge_sample{std::numeric_limits<float>::max()};
    TestSource huge{huge_sample, 1.0};
    sl::SecondSourceStage output_overflow{default_params, &huge};
    REQUIRE(output_overflow.prepare(context(scratch.data(), 2.0)).is_ok());
    std::array<float, 1U> unchanged{};
    const auto nonfinite = output_overflow.process(unchanged.data(), 1U, 0U, 1U);
    CHECK_FALSE(nonfinite.has_value());
    CHECK(unchanged[0] == 0.0F);
}

TEST_CASE("second source refuses gaps before provider or filter state changes", "[second-source]")
{
    constexpr std::array<float, 4U> samples{0.1F, 0.2F, -0.2F, -0.1F};
    TestSource source{samples, 1.0};
    sl::SecondSourceParams params{};
    params.frequency_offset = units::FrequencyDelta{10.0, units::Hz};
    sl::SecondSourceStage stage{params, &source};
    std::array<float, sl::engine_capacity_frames> scratch{};
    REQUIRE(stage.prepare(context(scratch.data())).is_ok());
    std::array<float, 8U> block{};
    const auto gap = stage.process(block.data(), 4U, 1U, 4U);
    CHECK_FALSE(gap.has_value());
    CHECK(source.reads() == 0U);
    const auto empty = stage.process(nullptr, 0U, 99U, 0U);
    REQUIRE(empty.has_value());
    CHECK(empty.value() == 0U);
    CHECK(source.reads() == 0U);
    const auto retry = stage.process(block.data(), 4U, 0U, 4U);
    REQUIRE(retry.has_value());
    CHECK(source.reads() == 1U);
}

TEST_CASE("second source propagates provider failures and can be prepared again", "[second-source]")
{
    constexpr std::array<float, 4U> samples{0.1F, 0.2F, -0.2F, -0.1F};
    TestSource source{samples, 1.0};
    const sl::SecondSourceParams params{};
    sl::SecondSourceStage stage{params, &source};
    std::array<float, sl::engine_capacity_frames> scratch{};
    REQUIRE(stage.prepare(context(scratch.data())).is_ok());
    source.fail(true);
    std::array<float, 4U> block{1.0F, 1.0F, 1.0F, 1.0F};
    const auto failed = stage.process(block.data(), block.size(), 0U, block.size());
    REQUIRE_FALSE(failed.has_value());
    CHECK(failed.error().code == m110::StatusCode::io_error);
    constexpr std::array<float, 4U> unchanged{1.0F, 1.0F, 1.0F, 1.0F};
    CHECK(block == unchanged);
    const auto latched = stage.process(block.data(), block.size(), 0U, block.size());
    REQUIRE_FALSE(latched.has_value());
    CHECK(latched.error().code == m110::StatusCode::io_error);
    CHECK(source.reads() == 1U);
    source.fail(false);
    REQUIRE(stage.prepare(context(scratch.data())).is_ok());
    const auto retried = stage.process(block.data(), block.size(), 0U, block.size());
    REQUIRE(retried.has_value());
    CHECK(block[0] == 1.1F);
}

TEST_CASE("second source rejects non-finite provider samples and fixed reads over one block", "[second-source]")
{
    constexpr std::array<float, 2U> bad_samples{0.25F, std::numeric_limits<float>::quiet_NaN()};
    TestSource source{bad_samples, 1.0};
    const sl::SecondSourceParams params{};
    sl::SecondSourceStage stage{params, &source};
    std::array<float, sl::engine_capacity_frames> scratch{};
    REQUIRE(stage.prepare(context(scratch.data())).is_ok());
    std::array<float, 2U> block{0.5F, 0.5F};
    const auto bad = stage.process(block.data(), block.size(), 0U, block.size());
    CHECK_FALSE(bad.has_value());
    CHECK(block[0] == 0.5F);
    CHECK(block[1] == 0.5F);
    std::array<float, sl::engine_block_frames + 1U> fixed_samples{};
    sl::FixedSecondarySource fixed{fixed_samples, 1.0};
    std::array<float, sl::engine_block_frames + 1U> fixed_output{};
    const auto too_wide = fixed.read(0U, fixed_output);
    CHECK_FALSE(too_wide.is_ok());
}

TEST_CASE("second source latches a post-shift output failure until prepare", "[second-source]")
{
    constexpr std::size_t frames = 256U;
    std::array<float, frames> samples{};
    samples.fill(2.0e38F);
    TestSource source{samples, 1.0};
    sl::SecondSourceParams params{};
    params.frequency_offset = units::FrequencyDelta{10.0, units::Hz};
    sl::SecondSourceStage stage{params, &source};
    std::array<float, sl::engine_capacity_frames> scratch{};
    REQUIRE(stage.prepare(context(scratch.data(), 2.0)).is_ok());
    std::array<float, frames> block{};
    block.fill(1.0e38F);
    const std::array<float, frames> unchanged = block;
    const auto failed = stage.process(block.data(), block.size(), 0U, block.size());
    REQUIRE_FALSE(failed.has_value());
    CHECK(block == unchanged);
    CHECK(source.reads() == 1U);
    const auto latched = stage.process(block.data(), block.size(), 0U, block.size());
    CHECK_FALSE(latched.has_value());
    CHECK(latched.error().message == failed.error().message);
    CHECK(source.reads() == 1U);
    CHECK(block == unchanged);
}

TEST_CASE("second source refuses oversized direct blocks before reading source", "[second-source]")
{
    constexpr std::array<float, 1U> samples{0.25F};
    TestSource source{samples, 1.0};
    const sl::SecondSourceParams params{};
    sl::SecondSourceStage stage{params, &source};
    std::array<float, sl::engine_capacity_frames> scratch{};
    REQUIRE(stage.prepare(context(scratch.data())).is_ok());
    std::array<float, sl::engine_capacity_frames> block{};
    const auto refused = stage.process(block.data(), block.size(), 0U, block.size());
    CHECK_FALSE(refused.has_value());
    CHECK(source.reads() == 0U);
    CHECK(stage.stats().component_frames == 0U);
    const auto retry = stage.process(block.data(), sl::engine_block_frames, 0U, block.size());
    CHECK(retry.has_value());
    CHECK(source.reads() == 1U);
}

TEST_CASE("second source configuration and processing do not allocate", "[second-source][allocation]")
{
    constexpr std::array<float, 32U> samples{0.1F, 0.2F, 0.3F, 0.4F};
    sl::FixedSecondarySource source{samples, 0.25};
    sl::SecondSourceParams params{};
    params.frequency_offset = units::FrequencyDelta{25.0, units::Hz};
    params.fading.enabled = true;
    params.fading.spread = units::FrequencyDelta{10.0, units::Hz};
    params.markov = markov_params(4.0, 3.0);
    sl::SecondSourceStage stage{params, &source};
    std::array<float, sl::engine_capacity_frames> scratch{};
    std::array<float, sl::engine_block_frames> block{};
    bool all_ok = false;
    std::uint64_t allocations = 0U;
    {
        const signal_lab_test::CountedRegion region;
        all_ok = stage.prepare(context(scratch.data())).is_ok();
        const auto result = stage.process(block.data(), block.size(), 0U, block.size());
        all_ok = all_ok && result.has_value();
        allocations = region.allocations();
    }

    CHECK(all_ok);
    CHECK(allocations == 0U);
}

TEST_CASE("second source fading is validated and adds only the shared Hilbert latency", "[second-source][fading]")
{
    sl::SecondSourceParams params{};
    params.fading.enabled = true;
    CHECK_FALSE(sl::validate_second_source_params(params).is_ok());
    params.fading.spread = units::FrequencyDelta{0.1, units::Hz};
    CHECK(sl::validate_second_source_params(params).is_ok());
    CHECK(sl::second_source_filter_latency_frames(params) == sl::hilbert_latency);
    params.fading.spread = units::FrequencyDelta{10.0, units::Hz};
    CHECK(sl::validate_second_source_params(params).is_ok());
    params.fading.spread = units::FrequencyDelta{10.0001, units::Hz};
    CHECK_FALSE(sl::validate_second_source_params(params).is_ok());
    params.fading.spread = units::FrequencyDelta{std::numeric_limits<double>::quiet_NaN(), units::Hz};
    CHECK_FALSE(sl::validate_second_source_params(params).is_ok());
    params.fading.enabled = false;
    CHECK(sl::validate_second_source_params(params).is_ok());
    CHECK(sl::second_source_filter_latency_frames(params).value() == 0U);

    for (const double offset : std::array<double, 2U>{75.0, -75.0})
    {
        params.frequency_offset = units::FrequencyDelta{offset, units::Hz};
        CHECK(sl::second_source_filter_latency_frames(params) == sl::hilbert_latency);
    }
}

TEST_CASE("second source fading is deterministic by explicit stream ID and partition independent", "[second-source][fading]")
{
    constexpr std::size_t total = 4096U;
    std::array<float, total> samples{};

    for (std::size_t index = 0U; index < samples.size(); ++index)
    {
        samples[index] = static_cast<float>(0.35 * std::sin(2.0 * 3.141592653589793 * 1250.0 * static_cast<double>(index) / 48000.0));
    }

    sl::SecondSourceParams params{};
    params.fading.enabled = true;
    params.fading.spread = units::FrequencyDelta{0.7, units::Hz};
    params.start_frame = 23U;
    params.source_start_frame = 17U;
    params.duration_frames = total - params.source_start_frame;
    TestSource same_a{samples, 0.25};
    TestSource same_b{samples, 0.25};
    TestSource partitioned_source{samples, 0.25};
    TestSource other_id_source{samples, 0.25};
    sl::SecondSourceStage a{params, &same_a};
    sl::SecondSourceStage b{params, &same_b};
    sl::SecondSourceStage partitioned{params, &partitioned_source};
    sl::SecondSourceStage other_id{params, &other_id_source};
    const auto rendered_total = static_cast<std::size_t>(params.start_frame + params.duration_frames);
    const auto first = render(a, sl::engine_block_frames, rendered_total, 0.5, same_a, 101U, 3U);
    CHECK(render(b, sl::engine_block_frames, rendered_total, 0.5, same_b, 101U, 99U) == first);
    CHECK(render(partitioned, 127U, rendered_total, 0.5, partitioned_source, 101U, 3U) == first);
    const auto different = render(other_id, 127U, rendered_total, 0.5, other_id_source, 102U, 3U);
    CHECK(different != first);
    CHECK(a.stats().component_frames == params.duration_frames);
    CHECK(partitioned.stats().component_frames == params.duration_frames);
}

TEST_CASE("second source fading begins at excerpt-local frame zero and advances through zero samples", "[second-source][fading]")
{
    constexpr std::size_t tone_frames = 4096U;
    constexpr std::size_t silent_prefix = 48000U;
    std::array<float, tone_frames> tone{};
    std::vector<float> with_silence(silent_prefix + tone_frames, 0.0F);

    for (std::size_t index = 0U; index < tone.size(); ++index)
    {
        tone[index] = static_cast<float>(0.4 * std::sin(2.0 * 3.141592653589793 * 1700.0 * static_cast<double>(index) / 48000.0));
        with_silence[silent_prefix + index] = tone[index];
    }

    sl::SecondSourceParams at_window_a{};
    at_window_a.fading.enabled = true;
    at_window_a.fading.spread = units::FrequencyDelta{1.2, units::Hz};
    at_window_a.start_frame = 19U;
    at_window_a.source_start_frame = 37U;
    at_window_a.duration_frames = tone_frames;
    sl::SecondSourceParams at_window_b = at_window_a;
    at_window_b.start_frame = 311U;
    at_window_b.source_start_frame = 0U;
    std::array<float, tone_frames + 37U> prefixed_tone{};
    prefixed_tone.fill(-0.1F);
    std::ranges::copy(tone, prefixed_tone.begin() + 37U);
    TestSource window_source_a{prefixed_tone, 0.3};
    TestSource window_source_b{tone, 0.3};
    sl::SecondSourceStage window_a{at_window_a, &window_source_a};
    sl::SecondSourceStage window_b{at_window_b, &window_source_b};
    const auto window_output_a = render(window_a, 127U, tone_frames + 400U, 0.6, window_source_a, 222U);
    const auto window_output_b = render(window_b, 127U, tone_frames + 400U, 0.6, window_source_b, 222U);
    CHECK(std::equal(window_output_a.begin() + at_window_a.start_frame, window_output_a.begin() + at_window_a.start_frame + tone_frames, window_output_b.begin() + at_window_b.start_frame));
    sl::SecondSourceParams delayed_fade = at_window_a;
    delayed_fade.start_frame = 0U;
    delayed_fade.source_start_frame = 0U;
    delayed_fade.duration_frames = with_silence.size();
    sl::SecondSourceParams fresh_fade = at_window_a;
    fresh_fade.source_start_frame = 0U;
    TestSource delayed_source{with_silence, 0.3};
    TestSource fresh_source{tone, 0.3};
    sl::SecondSourceStage delayed{delayed_fade, &delayed_source};
    sl::SecondSourceStage fresh{fresh_fade, &fresh_source};
    const auto delayed_output = render(delayed, 1024U, with_silence.size(), 0.6, delayed_source, 333U);
    const auto fresh_output = render(fresh, 1024U, tone_frames + at_window_a.start_frame, 0.6, fresh_source, 333U);
    const auto delayed_tone = std::span<const float>{delayed_output}.subspan(silent_prefix, tone_frames);
    const auto fresh_tone = std::span<const float>{fresh_output}.subspan(at_window_a.start_frame, tone_frames);
    double difference_energy = 0.0;

    for (std::size_t index = sl::hilbert_latency.value(); index < tone_frames; ++index)
    {
        const double difference = static_cast<double>(delayed_tone[index]) - fresh_tone[index];
        difference_energy += difference * difference;
    }

    CHECK(difference_energy > 1.0e-4);
}

TEST_CASE("second source fading is independent of the primary channel and disabled fading is exact legacy", "[second-source][fading]")
{
    constexpr std::size_t frames = 1024U;
    std::array<float, frames> samples{};

    for (std::size_t index = 0U; index < samples.size(); ++index)
    {
        samples[index] = static_cast<float>(0.25 * std::cos(2.0 * 3.141592653589793 * 1900.0 * static_cast<double>(index) / 48000.0));
    }

    TestSource source_a{samples, 0.2};
    TestSource source_b{samples, 0.2};
    sl::SecondSourceParams fading{};
    fading.fading.enabled = true;
    fading.fading.spread = units::FrequencyDelta{0.5, units::Hz};
    sl::SecondSourceStage stage_a{fading, &source_a};
    sl::SecondSourceStage stage_b{fading, &source_b};
    std::array<float, sl::engine_capacity_frames> scratch_a{};
    std::array<float, sl::engine_capacity_frames> scratch_b{};
    REQUIRE(stage_a.prepare(context(scratch_a.data(), 0.4, 444U)).is_ok());
    REQUIRE(stage_b.prepare(context(scratch_b.data(), 0.4, 444U)).is_ok());
    std::array<float, frames> zero_primary{};
    std::array<float, frames> nonzero_primary{};
    nonzero_primary.fill(0.1F);
    REQUIRE(stage_a.process(zero_primary.data(), frames, 0U, frames).has_value());
    REQUIRE(stage_b.process(nonzero_primary.data(), frames, 0U, frames).has_value());

    for (std::size_t index = 0U; index < frames; ++index)
    {
        CHECK(nonzero_primary[index] - zero_primary[index] == Catch::Approx(0.1F).margin(2.0e-7F));
    }

    CHECK(stage_a.stats().component_energy == stage_b.stats().component_energy);
    TestSource legacy_source_a{samples, 0.2};
    TestSource legacy_source_b{samples, 0.2};
    sl::SecondSourceParams legacy_default{};
    sl::SecondSourceParams disabled_fade{};
    disabled_fade.fading.spread = units::FrequencyDelta{std::numeric_limits<double>::quiet_NaN(), units::Hz};
    sl::SecondSourceStage legacy_a{legacy_default, &legacy_source_a};
    sl::SecondSourceStage legacy_b{disabled_fade, &legacy_source_b};
    CHECK(render(legacy_a, 127U, frames, 0.4, legacy_source_a) == render(legacy_b, 127U, frames, 0.4, legacy_source_b));
}

TEST_CASE("second source fading composes with either offset sign using one shared filter delay", "[second-source][fading]")
{
    constexpr std::size_t total = 1024U;
    constexpr std::uint64_t start = 29U;
    std::array<float, total> samples{};

    for (std::size_t index = 0U; index < samples.size(); ++index)
    {
        samples[index] = static_cast<float>(0.2 * std::sin(2.0 * 3.141592653589793 * 2200.0 * static_cast<double>(index) / 48000.0));
    }

    for (const double offset_hz : std::array<double, 2U>{41.0, -41.0})
    {
        sl::SecondSourceParams params{};
        params.start_frame = start;
        params.duration_frames = total;
        params.frequency_offset = units::FrequencyDelta{offset_hz, units::Hz};
        params.fading.enabled = true;
        params.fading.spread = units::FrequencyDelta{0.8, units::Hz};
        CHECK(sl::second_source_filter_latency_frames(params) == sl::hilbert_latency);
        TestSource whole_source{samples, 0.2};
        TestSource pieces_source{samples, 0.2};
        sl::SecondSourceStage whole{params, &whole_source};
        sl::SecondSourceStage pieces{params, &pieces_source};
        const auto expected = render(whole, 1024U, total + start + 5U, 0.5, whole_source, 555U);
        CHECK(render(pieces, 1U, total + start + 5U, 0.5, pieces_source, 555U) == expected);
        CHECK(std::ranges::all_of(expected.begin(), expected.begin() + start, [](float value) { return value == 0.0F; }));
    }
}

TEST_CASE("second source Markov gate has a pinned initial ON/OFF vector", "[second-source][markov]")
{
    constexpr std::array<float, 20U> samples{0.25F, 0.25F, 0.25F, 0.25F, 0.25F, 0.25F, 0.25F, 0.25F, 0.25F, 0.25F, 0.25F, 0.25F, 0.25F, 0.25F, 0.25F, 0.25F, 0.25F, 0.25F, 0.25F, 0.25F};
    TestSource source{samples, 0.25};
    sl::SecondSourceParams params{};
    params.si = units::Ratio::from_db(0.0);
    params.duration_frames = samples.size();
    params.markov = markov_params(4.0, 3.0);
    sl::SecondSourceStage stage{params, &source};
    const auto output = render(stage, samples.size(), samples.size(), 0.25, source, 0U, 7U, 123U);
    // Literal family-10 sequence for seed 123/index 7: ON=3, OFF=2, ON=4,
    // OFF=1, ON=3, OFF=2, ON=4, OFF=3.
    constexpr std::array<std::uint64_t, 8U> dwells{3U, 2U, 4U, 1U, 3U, 2U, 4U, 3U};
    std::size_t frame{};
    bool on = true;

    for (const std::uint64_t dwell : dwells)
    {
        for (std::uint64_t index = 0U; index < dwell && frame < samples.size(); ++index)
        {
            CHECK(output[frame] == (on ? 0.25F : 0.0F));
            ++frame;
        }

        if (frame == samples.size())
        {
            break;
        }

        on = !on;
    }

    CHECK(frame == samples.size());
}

TEST_CASE("second source Markov gate follows its window and gates after the continuing filter", "[second-source][markov]")
{
    constexpr std::size_t active_frames = 512U;
    constexpr std::uint64_t destination_start = 37U;
    constexpr std::size_t total_frames = destination_start + active_frames + 41U;
    constexpr std::uint64_t seed = 123U;
    constexpr std::uint32_t legacy_index = 7U;
    constexpr std::uint32_t stable_id = 0x2A17U;
    std::array<float, active_frames> samples{};

    for (std::size_t index = 0U; index < samples.size(); ++index)
    {
        samples[index] = static_cast<float>(0.25 * std::sin(2.0 * 3.141592653589793 * 1200.0 * static_cast<double>(index) / static_cast<double>(sl::sample_rate_hz)));
    }

    struct FilterCase
    {
        double offset_hz;
        bool fading;
    };

    constexpr std::array cases{FilterCase{0.0, false}, FilterCase{75.0, false}, FilterCase{-75.0, false}, FilterCase{0.0, true}, FilterCase{75.0, true}, FilterCase{-75.0, true}};

    constexpr std::array block_sizes{1U, 127U, 2048U};

    for (const FilterCase filter : cases)
    {
        sl::SecondSourceParams gated_params{};
        gated_params.si = units::Ratio::from_db(0.0);
        gated_params.start_frame = destination_start;
        gated_params.duration_frames = active_frames;
        gated_params.frequency_offset = units::FrequencyDelta{filter.offset_hz, units::Hz};
        gated_params.fading.enabled = filter.fading;
        gated_params.fading.spread = filter.fading ? units::FrequencyDelta{0.7, units::Hz} : units::FrequencyDelta{};
        gated_params.markov = markov_params(13.0, 9.0);
        CHECK(sl::validate_second_source_params(gated_params).is_ok());
        sl::SecondSourceParams ungated_params = gated_params;
        ungated_params.markov = sl::MarkovGateParams{};
        CHECK(sl::second_source_filter_latency_frames(gated_params) == sl::second_source_filter_latency_frames(ungated_params));
        TestSource reference_source{samples, 0.25};
        sl::SecondSourceStage reference_stage{ungated_params, &reference_source};
        const auto reference = render(reference_stage, 2048U, total_frames, 0.25, reference_source, stable_id, legacy_index, seed);
        TestSource local_source{samples, 0.25};
        sl::SecondSourceParams local_params = gated_params;
        local_params.start_frame = 0U;
        sl::SecondSourceStage local_stage{local_params, &local_source};
        const auto local = render(local_stage, 127U, active_frames, 0.25, local_source, stable_id, legacy_index, seed);
        const sl::StageRandomKey key = sl::resolve_stage_random_key(seed, legacy_index, stable_id);
        sl::MarkovGate gate{};
        REQUIRE(gate.configure(gated_params.markov, key.seed, key.index).is_ok());
        std::array<bool, active_frames> expected_on{};

        for (bool& on : expected_on)
        {
            on = gate.on();
            (void)gate.advance();
        }

        std::vector<float> partition_reference;

        for (const std::size_t block_size : block_sizes)
        {
            TestSource source{samples, 0.25};
            sl::SecondSourceStage stage{gated_params, &source};
            const auto output = render(stage, block_size, total_frames, 0.25, source, stable_id, legacy_index, seed);

            if (partition_reference.empty())
            {
                partition_reference = output;
            }
            else
            {
                CHECK(output == partition_reference);
            }

            CHECK(std::equal(output.begin() + destination_start, output.begin() + destination_start + active_frames, local.begin()));
            CHECK(std::all_of(output.begin(), output.begin() + destination_start, [](float value) { return value == 0.0F; }));

            CHECK(std::all_of(output.begin() + destination_start + active_frames, output.end(), [](float value) { return value == 0.0F; }));

            for (std::size_t frame = 0U; frame < active_frames; ++frame)
            {
                const float actual = output[static_cast<std::size_t>(destination_start) + frame];
                CHECK(actual == (expected_on[frame] ? reference[static_cast<std::size_t>(destination_start) + frame] : 0.0F));
            }
        }

        TestSource same_id_source{samples, 0.25};
        sl::SecondSourceStage same_id_stage{gated_params, &same_id_source};
        CHECK(render(same_id_stage, 127U, total_frames, 0.25, same_id_source, stable_id, 99U, seed) == partition_reference);
    }
}
