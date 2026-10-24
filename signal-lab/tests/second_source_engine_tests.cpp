// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

#include "allocation_counter.hpp"
#include "signal_lab/mixer.hpp"
#include "signal_lab/secondary_source.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <span>

namespace sl = signal_lab;

namespace
{

constexpr m110::Status provider_failure{m110::StatusCode::io_error, "test secondary source read failed"};

class FailingSource final : public sl::SecondarySource
{
public:
    [[nodiscard]] std::uint64_t frames() const noexcept override { return 4096U; }

    [[nodiscard]] double reference_rms() const noexcept override { return 0.25; }

    [[nodiscard]] m110::Status read(std::uint64_t, std::span<float>) noexcept override
    {
        ++reads;
        return provider_failure;
    }

    std::uint32_t reads{};
};

[[nodiscard]] sl::Scenario overlay_scenario()
{
    sl::Scenario scenario{};
    scenario.source_gain_db = -6.0;
    scenario.saturate = false;
    scenario.stage_count = 1U;
    scenario.stages[0].type = sl::StageType::second_source;
    scenario.second_source.si = sl::units::Ratio{12.0, sl::units::dB};
    return scenario;
}

} // namespace

TEST_CASE("second source engine: missing provider fails before playback and reconfigure recovers", "[second-source][engine]")
{
    sl::Engine engine{};
    const auto scenario = overlay_scenario();
    const auto refused = engine.configure(scenario, 0.25, 1024U);
    CHECK_FALSE(refused.is_ok());
    CHECK_FALSE(engine.configured());
    engine.configure_passthrough();
    CHECK(engine.configured());
}

TEST_CASE("second source engine: desired gain and secondary calibration set independent S/I", "[second-source][engine]")
{
    sl::Engine engine{};
    const auto scenario = overlay_scenario();
    std::array<float, 1024U> desired{};
    std::array<float, 1024U> secondary{};
    desired.fill(0.25F);
    secondary.fill(0.125F);
    sl::FixedSecondarySource provider{secondary, 0.125};
    sl::StageResources resources{};
    resources.secondary_source = &provider;
    std::array<float, sl::engine_capacity_frames> output{};
    REQUIRE(engine.configure(scenario, 0.25, desired.size(), resources).is_ok());
    REQUIRE(engine.process(desired, output).has_value());
    const double desired_level = 0.25 * sl::det::db_to_amplitude(-6.0);
    const double added_level = desired_level * sl::det::db_to_amplitude(-12.0);
    const float expected = static_cast<float>(static_cast<float>(desired_level) + static_cast<float>(added_level));

    for (std::size_t index = 0; index < desired.size(); ++index)
    {
        CHECK(output[index] == expected);
    }

    REQUIRE(engine.stage(0U) != nullptr);
    const auto& stats = engine.stage(0U)->stats();
    CHECK(stats.component_frames == desired.size());
    CHECK(std::abs(10.0 * std::log10(desired_level * desired_level / (stats.component_energy / static_cast<double>(stats.component_frames))) - 12.0) < 0.001);
    CHECK(engine.stats().arena_bytes_used == 0U);
}

TEST_CASE("second source engine: provider failure latches without committing output statistics", "[second-source][engine]")
{
    sl::Engine engine{};
    const auto scenario = overlay_scenario();
    FailingSource provider{};
    sl::StageResources resources{};
    resources.secondary_source = &provider;
    std::array<float, 127U> input{};
    std::array<float, sl::engine_capacity_frames> output{};
    REQUIRE(engine.configure(scenario, 0.25, 1024U, resources).is_ok());
    const auto failed = engine.process(input, output);
    REQUIRE_FALSE(failed.has_value());
    CHECK(sl::same_failure(failed.status(), provider_failure));
    CHECK(engine.stats().frames_out == 0U);
    const auto repeated = engine.process(input, output);
    REQUIRE_FALSE(repeated.has_value());
    CHECK(sl::same_failure(repeated.status(), provider_failure));
    CHECK(provider.reads == 1U);
}

TEST_CASE("second source engine: configuration and shifted processing allocate no memory", "[second-source][engine][allocation]")
{
    sl::Engine engine{};
    auto scenario = overlay_scenario();
    scenario.second_source.frequency_offset = sl::units::FrequencyDelta{75.0, sl::units::Hz};
    std::array<float, 1024U> secondary{};
    secondary.fill(0.125F);
    sl::FixedSecondarySource provider{secondary, 0.125};
    sl::StageResources resources{};
    resources.secondary_source = &provider;
    std::array<float, 1024U> input{};
    std::array<float, sl::engine_capacity_frames> output{};
    m110::Status configured{};
    m110::Result<std::size_t> result{std::size_t{0}};
    std::uint64_t allocations{};
    {
        signal_lab_test::CountedRegion counted{};
        configured = engine.configure(scenario, 0.25, input.size(), resources);
        result = engine.process(input, output);
        allocations = counted.allocations();
    }

    REQUIRE(configured.is_ok());
    REQUIRE(result.has_value());
    CHECK(allocations == 0U);
}
