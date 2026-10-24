// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// Adversarial API checks: invalid inputs must fail before indexing, conversion
// or artifact publication. No audio devices or target hardware are involved.
#include "signal_lab/live_control.hpp"
#include "signal_lab/mixer.hpp"
#include "waveform-source/wav_generator.hpp"

#include <array>
#include <cstdio>
#include <cstring>
#include <limits>
#include <type_traits>

namespace
{
int failures{};
void check(bool condition, const char* label) noexcept
{
    if (!condition)
    {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", label);
    }
}

class EmptyPayload final : public waveform_source::ByteSource
{
public:
    std::size_t read(std::uint8_t*, std::size_t) noexcept override { return 0U; }
};

class FaultyEncoder final : public waveform_source::Encoder
{
public:
    waveform_source::Status configure(std::string_view, waveform_source::ByteSource&, std::size_t) noexcept override { return {}; }

    waveform_source::Status status() const noexcept override { return {}; }

    void stop() noexcept override {}

    std::uint64_t total_frames() const noexcept override { return 1U; }

    m110::Result<std::size_t> read(std::int16_t*, std::size_t capacity) noexcept override { return capacity + 1U; }
};

class NonfiniteEncoder final : public waveform_source::Encoder
{
public:
    waveform_source::Status configure(std::string_view, waveform_source::ByteSource&, std::size_t) noexcept override { return {}; }

    waveform_source::Status status() const noexcept override { return {}; }

    void stop() noexcept override {}

    std::uint64_t total_frames() const noexcept override { return 1U; }

    m110::Result<std::size_t> read(std::int16_t*, std::size_t) noexcept override { return std::size_t{0U}; }

    std::size_t read_float(float* output, std::size_t) noexcept override
    {
        output[0] = std::numeric_limits<float>::quiet_NaN();
        return 1U;
    }
};

// A legacy encoder whose PCM16 reader fails.
class UnreadableEncoder final : public waveform_source::Encoder
{
public:
    waveform_source::Status configure(std::string_view, waveform_source::ByteSource&, std::size_t) noexcept override { return {}; }

    [[nodiscard]] waveform_source::Status status() const noexcept override { return {}; }

    void stop() noexcept override {}

    [[nodiscard]] std::uint64_t total_frames() const noexcept override { return 1U; }

    m110::Result<std::size_t> read(std::int16_t*, std::size_t) noexcept override { return waveform_source::Status{m110::StatusCode::io_error, "the test encoder cannot read"}; }
};

class CountingSink final : public waveform_source::ByteSink
{
public:
    m110::Status write(const std::uint8_t*, std::size_t count) noexcept override
    {
        bytes += count;
        return m110::Status::success();
    }

    std::size_t bytes{};
};
} // namespace

int main()
{
    static_assert(!std::is_copy_constructible_v<signal_lab::Engine>);
    static_assert(!std::is_move_constructible_v<signal_lab::Engine>);
    static_assert(!std::is_copy_constructible_v<waveform_source::WavGenerator>);
    signal_lab::Engine engine;
    signal_lab::Scenario scenario{};
    check(engine.scenario() == nullptr, "unconfigured scenario access is nullable");
    scenario.stage_count = signal_lab::max_stages + 1U;
    check(engine.configure(scenario, 0.1, 100U).code == m110::StatusCode::invalid_configuration, "oversized direct stage count rejected");
    scenario = signal_lab::Scenario{};
    check(engine.configure(scenario, std::numeric_limits<double>::infinity(), 100U).code == m110::StatusCode::invalid_argument, "infinite reference rejected");
    scenario.stage_count = 1U;
    scenario.stages[0].type = signal_lab::StageType::fade;
    scenario.stages[0].fade.start_count = signal_lab::max_events + 1U;
    check(engine.configure(scenario, 0.1, 100U).code == m110::StatusCode::invalid_configuration, "oversized direct fade count rejected");
    alignas(signal_lab::impairment_alignment) std::array<unsigned char, signal_lab::impairment_arena_bytes + 1U> storage{};
    check(signal_lab::same_failure(signal_lab::construct_cw({}, storage.data() + 1U, storage.size() - 1U).error(), signal_lab::placement_refused), "misaligned placement rejected");

    for (const char* json : {"{\"seed\":1.5}", "{\"seed\":18446744073709551616}", "{\"source_gain_db\":1e999}"})
    {
        check(signal_lab::parse_scenario(json, std::strlen(json), scenario).code == m110::StatusCode::invalid_argument, "unsafe JSON numeric input rejected");
    }

    engine.configure_passthrough();
    std::array<float, 2U> input{0.1F, std::numeric_limits<float>::quiet_NaN()};
    std::array<float, signal_lab::engine_capacity_frames> output{};
    output.fill(0.25F);
    const m110::Result<std::size_t> nonfinite_block = engine.process(std::span<const float>{input}, std::span<float>{output});
    check(!nonfinite_block.has_value() && signal_lab::same_failure(nonfinite_block.error(), signal_lab::engine_failure::nonfinite_input) &&
              signal_lab::same_failure(engine.status(), signal_lab::engine_failure::nonfinite_input),
          "nonfinite input latches failure");
    check(output[0] == 0.25F && engine.stats().frames_in == 0U, "bad input commits neither samples nor input digest");
    FaultyEncoder faulty;
    std::array<float, 1U> guard{0.5F};
    check(faulty.read_float(guard.data(), guard.size()) == 0U && guard[0] == 0.5F, "over-reported legacy count cannot overrun adapter");
    UnreadableEncoder unreadable;
    const float untouched = guard[0];
    check(unreadable.read_float(guard.data(), guard.size()) == 0U && guard[0] == untouched, "a failed legacy read gives no frame through the adapter");
    EmptyPayload payload;
    NonfiniteEncoder nonfinite;
    CountingSink sink;
    waveform_source::WavGenerator writer;
    check(writer.begin(nonfinite, "test", payload, 0U, sink, 0U).is_ok(), "nonfinite source can start");
    check(writer.step() == waveform_source::JobState::failed && sink.bytes == 44U, "nonfinite encoder output never writes PCM");
    std::printf("safety contracts: %d failures\n", failures);
    return failures == 0 ? 0 : 1;
}
